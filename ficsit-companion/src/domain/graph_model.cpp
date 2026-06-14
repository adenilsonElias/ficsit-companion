#include "domain/graph_model.hpp"

#include "infra/editor_backend.hpp"
#include "domain/graph_item_resolve.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/rate_solver.hpp"
#include "domain/recipe.hpp"
#include "domain/vehicle_route.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace
{
    // Re-solve a vehicle route pool, seeding from a pool member's active cargo
    // pin that already carries a rate (so the pool settles around an existing
    // value). Returns false only if a solve ran and was rejected. Does nothing
    // (returns true) when the pool carries no rate yet — nothing to balance.
    bool ResolveRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                          std::vector<std::unique_ptr<Link>>& links,
                          const std::vector<VehicleStationNode*>& pool,
                          float& error_time, float error_flow_duration)
    {
        for (VehicleStationNode* s : pool)
        {
            for (const auto& p : s->ins)
            {
                if (IsActiveCargoPin(p.get()) && p->item != nullptr && p->current_rate.GetNumerator() != 0)
                {
                    return RateSolver::Solve(nodes, links, p.get(), p->current_rate, error_time, error_flow_duration);
                }
            }
            for (const auto& p : s->outs)
            {
                if (IsActiveCargoPin(p.get()) && p->item != nullptr && p->current_rate.GetNumerator() != 0)
                {
                    return RateSolver::Solve(nodes, links, p.get(), p->current_rate, error_time, error_flow_duration);
                }
            }
        }
        return true;
    }

    // Carry cargo item types across a route pool, then re-balance it. Returns
    // false only if the balance solve was rejected.
    bool SyncRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const std::vector<VehicleStationNode*>& pool,
                       float& error_time, float error_flow_duration)
    {
        VehicleRoute::PropagateCargoItems(pool);
        return ResolveRoutePool(nodes, links, pool, error_time, error_flow_duration);
    }
}

GraphModel::GraphModel(IEditorBackend& editor) : editor(editor)
{
}

unsigned long long int GraphModel::GetNextId()
{
    return next_id++;
}

Pin* GraphModel::FindPin(ax::NodeEditor::PinId id) const
{
    if (id == ax::NodeEditor::PinId::Invalid)
    {
        return nullptr;
    }

    for (const auto& n : nodes)
    {
        for (const auto& p : n->ins)
        {
            if (p->id == id)
            {
                return p.get();
            }
        }

        for (const auto& p : n->outs)
        {
            if (p->id == id)
            {
                return p.get();
            }
        }

        // Vehicle stations carry a plug pin outside ins/outs.
        if (n->IsLogistics())
        {
            LogisticsNode* l = static_cast<LogisticsNode*>(n.get());
            if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                l->logistics_kind == LogisticsNode::Kind::TrainStation)
            {
                Pin* plug = static_cast<VehicleStationNode*>(l)->plug.get();
                if (plug != nullptr && plug->id == id)
                {
                    return plug;
                }
            }
        }
    }

    return nullptr;
}

void GraphModel::CreateLink(Pin* start, Pin* end, bool trigger_update, float& error_time, float error_flow_duration)
{
    // Make sure start is always an output and end an input
    Pin* real_end = end->direction == ax::NodeEditor::PinKind::Input ? end : start;
    Pin* real_start = start->direction == ax::NodeEditor::PinKind::Output ? start : end;
    links.emplace_back(std::make_unique<Link>(GetNextId(), real_start, real_end));
    Link* created = links.back().get();
    // Route link (plug<->plug): tracked per-station, invisible to the solver.
    // Leave Pin::link null on both plugs.
    if (IsVehiclePlug(real_start) && IsVehiclePlug(real_end))
    {
        VehicleStationNode* s_start = static_cast<VehicleStationNode*>(real_start->node);
        s_start->route_links.push_back(created);
        static_cast<VehicleStationNode*>(real_end->node)->route_links.push_back(created);
        // Interactive connect: carry cargo items across the new route and balance
        // it; a rejected solve (over-constrained route) deletes the link, exactly
        // like a belt-link creation. Bulk restore (session load / group paste,
        // trigger_update == false) keeps the serialized state authoritative and
        // only records the route link.
        if (trigger_update)
        {
            try
            {
                if (!SyncRoutePool(nodes, links, VehicleRoute::FindPool(s_start), error_time, error_flow_duration))
                {
                    DeleteLink(created->id);
                    return;
                }
            }
            catch (const std::runtime_error&)
            {
                DeleteLink(created->id);
                fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                error_time = error_flow_duration;
                return;
            }
        }
        return;
    }
    start->link = created;
    end->link = created;
    if (trigger_update && start->current_rate != end->current_rate)
    {
        try
        {
            if (!RateSolver::Solve(nodes, links, start, start->current_rate, error_time, error_flow_duration))
            {
                DeleteLink(links.back()->id);
                return;
            }
        }
        catch (const std::runtime_error&)
        {
            DeleteLink(links.back()->id);
            fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
            error_time = error_flow_duration;
            return;
        }
    }
    // Set lock state
    if (start->GetLocked() || end->GetLocked())
    {
        start->SetLocked(true);
        end->SetLocked(true);
    }
    // Set items for organizer nodes
    if (start->node->IsOrganizer())
    {
        if (OrganizerNode* organizer_node = static_cast<OrganizerNode*>(start->node); organizer_node->item == nullptr)
        {
            organizer_node->ChangeItem(end->item);
        }
    }
    if (end->node->IsOrganizer())
    {
        if (OrganizerNode* organizer_node = static_cast<OrganizerNode*>(end->node); organizer_node->item == nullptr)
        {
            organizer_node->ChangeItem(start->item);
        }
    }
    if (real_end->node->IsSink())
    {
        real_end->item = real_start->item;
    }
    if (real_end->node->IsLogistics())
    {
        // Storage is the destination: its input pin carries the upstream item.
        // The input decides the storage's material, so push it onto the outputs,
        // overriding any item an output may have adopted from a downstream node.
        // The dedicated fuel inlet (Truck/Train station) is isolated from the
        // cargo flow, so a fuel connection sets only the fuel pin and never the
        // outputs.
        real_end->item = real_start->item;
        if (real_end->item != nullptr && !IsFuelPin(real_end))
        {
            for (const auto& p : real_end->node->outs)
            {
                p->item = real_end->item;
            }
        }
    }
    if (real_start->node->IsLogistics())
    {
        // Storage is the source: its output carries the storage's material, which
        // the input decides. Only fall back to the downstream node's item when no
        // input item is known yet, so a downstream pin never overrides the input.
        // The fuel inlet is skipped: it does not feed the cargo outputs.
        const Item* in_item = nullptr;
        for (const auto& p : real_start->node->ins)
        {
            if (IsFuelPin(p.get())) continue;
            if (p->item != nullptr) { in_item = p->item; break; }
        }
        real_start->item = in_item != nullptr ? in_item : real_end->item;
    }
    // Interactive belt edits only: if this belt (re)typed a cargo pin of a
    // vehicle station that is already in a route pool, carry the item across the
    // route and re-balance so the far side follows. Best-effort — a rejected
    // re-solve leaves rates as they were (the belt link itself is valid
    // regardless of route balance). Bulk restore (trigger_update == false) leaves
    // the serialized state untouched.
    if (trigger_update)
    {
        auto sync_route = [&](Node* node) {
            if (node == nullptr || !node->IsLogistics()) return;
            LogisticsNode* l = static_cast<LogisticsNode*>(node);
            if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
                l->logistics_kind != LogisticsNode::Kind::TrainStation) return;
            VehicleStationNode* v = static_cast<VehicleStationNode*>(l);
            std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(v);
            if (pool.size() < 2) return; // not connected to another station
            try
            {
                SyncRoutePool(nodes, links, pool, error_time, error_flow_duration);
            }
            catch (const std::runtime_error&)
            {
                fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
                error_time = error_flow_duration;
            }
        };
        sync_route(real_end->node);
        sync_route(real_start->node);
    }
    // Extractors only inherit a resource from a downstream chain when they
    // don't already have one - the user picks the resource explicitly via the
    // Miner submenu, and silently overwriting that choice was causing
    // hard-to-debug routing problems (e.g. a Splitter feeding a different ore
    // chain would swap the miner's resource out from under the user).
    if (real_start->node->IsExtractor())
    {
        if (ExtractorNode* ex = static_cast<ExtractorNode*>(real_start->node);
            ex != nullptr && ex->resource == nullptr)
        {
            if (const Item* downstream = ResolveItemThroughChain(real_end);
                downstream != nullptr)
            {
                ex->ChangeResource(downstream, [this] { return GetNextId(); });
            }
        }
    }
    // Backward propagation: if this new link ended up giving an organizer a
    // concrete item (via the auto-assign above), push that item upstream so
    // any miner sitting on the other side of a Merger/Splitter inherits it.
    auto push_upstream = [&](Node* node, const Item* item_hint = nullptr) {
        if (node == nullptr || (!node->IsOrganizer() && !node->IsLogistics())) return;
        const Item* it = item_hint;
        if (it == nullptr && node->IsOrganizer())
        {
            it = static_cast<OrganizerNode*>(node)->item;
        }
        PropagateExtractorResourceUpstream(node, it, [this] { return GetNextId(); });
    };
    push_upstream(start->node, start->item);
    push_upstream(end->node, end->item);
}

void GraphModel::DeleteLink(ax::NodeEditor::LinkId id)
{
    editor.DeleteLink(id);
    auto it = std::find_if(links.begin(), links.end(), [id](const std::unique_ptr<Link>& link) { return link->id == id; });
    if (it != links.end())
    {
        // Route link (plug<->plug): unindex from each station, no Pin::link to clear.
        if (Link* l = it->get(); IsVehiclePlug(l->start) && IsVehiclePlug(l->end))
        {
            // Cutting this link may split the pool into two; re-settle both ends.
            VehicleStationNode* end_a = static_cast<VehicleStationNode*>(l->start->node);
            VehicleStationNode* end_b = static_cast<VehicleStationNode*>(l->end->node);
            auto unindex = [&](Pin* plug) {
                auto& rl = static_cast<VehicleStationNode*>(plug->node)->route_links;
                rl.erase(std::remove(rl.begin(), rl.end(), l), rl.end());
            };
            unindex(l->start);
            unindex(l->end);
            links.erase(it);
            // Re-settle each resulting pool around an existing rate. A rejected
            // solve here is best-effort: leave rates as they were.
            float error_time = 0.0f;
            try
            {
                ResolveRoutePool(nodes, links, VehicleRoute::FindPool(end_a), error_time, editor.GetFlowDuration());
                ResolveRoutePool(nodes, links, VehicleRoute::FindPool(end_b), error_time, editor.GetFlowDuration());
            }
            catch (const std::runtime_error&)
            {
                fprintf(stderr, "Propagation error, please report this issue on github or discord\n");
            }
            return;
        }
        if (Pin* start = (*it)->start; start != nullptr)
        {
            start->link = nullptr;
            // If either end was an organizer node, check the name is still valid
            if (start->node->IsOrganizer())
            {
                static_cast<OrganizerNode*>(start->node)->RemoveItemIfNotForced();
            }
            else if (start->node->IsLogistics())
            {
                start->item = nullptr;
                start->current_rate = 0;
            }
        }
        if (Pin* end = (*it)->end; end != nullptr)
        {
            end->link = nullptr;
            // If either end was an organizer node, check the name is still valid
            if (end->node->IsOrganizer())
            {
                static_cast<OrganizerNode*>(end->node)->RemoveItemIfNotForced();
            }
            else if (end->node->IsSink())
            {
                end->item = nullptr;
                end->current_rate = 0;
            }
            else if (end->node->IsLogistics())
            {
                end->item = nullptr;
                end->current_rate = 0;
            }
        }
        links.erase(it);
    }
}

void GraphModel::DeleteNode(ax::NodeEditor::NodeId id)
{
    editor.DeleteNode(id);
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const std::unique_ptr<Node>& n) { return n->id == id; });
    if (it != nodes.end())
    {
        for (auto& p : (*it)->ins)
        {
            if (p->link != nullptr)
            {
                DeleteLink(p->link->id);
            }
        }
        for (auto& p : (*it)->outs)
        {
            if (p->link != nullptr)
            {
                DeleteLink(p->link->id);
            }
        }
        // Vehicle station: delete the plug's route links (held outside ins/outs).
        if ((*it)->IsLogistics())
        {
            LogisticsNode* l = static_cast<LogisticsNode*>(it->get());
            if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                l->logistics_kind == LogisticsNode::Kind::TrainStation)
            {
                auto* v = static_cast<VehicleStationNode*>(l);
                // Copy ids first: DeleteLink mutates route_links.
                std::vector<ax::NodeEditor::LinkId> ids;
                for (Link* rl : v->route_links) ids.push_back(rl->id);
                for (auto rid : ids) DeleteLink(rid);
            }
        }
        nodes.erase(it);
    }
}
