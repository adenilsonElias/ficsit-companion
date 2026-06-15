#include "domain/vehicle_route.hpp"

#include "domain/graph_item_resolve.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/rate_solver.hpp"
#include "domain/recipe.hpp"

#include <unordered_set>

namespace
{
    // The other plug's owning station for a route link, given one endpoint plug.
    VehicleStationNode* OtherStation(const Link* link, const VehicleStationNode* self)
    {
        if (link == nullptr) return nullptr;
        const Pin* other = (link->start != nullptr && link->start->node == self) ? link->end : link->start;
        if (other == nullptr || other->node == nullptr) return nullptr;
        Node* n = other->node;
        if (!n->IsLogistics()) return nullptr;
        LogisticsNode* l = static_cast<LogisticsNode*>(n);
        if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
            l->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
        return static_cast<VehicleStationNode*>(l);
    }
}

namespace VehicleRoute
{
    std::vector<VehicleStationNode*> FindPool(VehicleStationNode* origin)
    {
        std::vector<VehicleStationNode*> pool;
        if (origin == nullptr) return pool;
        std::unordered_set<VehicleStationNode*> seen;
        std::vector<VehicleStationNode*> queue{ origin };
        while (!queue.empty())
        {
            VehicleStationNode* n = queue.back();
            queue.pop_back();
            if (!seen.insert(n).second) continue;
            pool.push_back(n);
            for (const Link* l : n->route_links)
            {
                if (VehicleStationNode* o = OtherStation(l, n))
                {
                    if (seen.find(o) == seen.end()) queue.push_back(o);
                }
            }
        }
        return pool;
    }

    std::vector<ItemBalance> SummarizePool(const std::vector<VehicleStationNode*>& pool)
    {
        // Preserve first-seen item order while accumulating.
        std::vector<ItemBalance> out;
        auto slot = [&](const Item* it) -> ItemBalance& {
            for (auto& b : out) if (b.item == it) return b;
            out.push_back(ItemBalance{ it, FractionalNumber(0, 1), FractionalNumber(0, 1) });
            return out.back();
        };
        for (VehicleStationNode* s : pool)
        {
            if (s->mode == VehicleStationNode::Mode::Load)
            {
                // Cargo inputs only: skip the fuel inlet (last input).
                for (size_t i = 0; i + 1 < s->ins.size(); ++i)
                {
                    const Pin* p = s->ins[i].get();
                    if (p->item != nullptr) slot(p->item).supply += p->current_rate;
                }
            }
            else
            {
                for (const auto& p : s->outs)
                {
                    if (p->item != nullptr) slot(p->item).demand += p->current_rate;
                }
            }
        }
        return out;
    }

    std::vector<PoolItemGroup> GroupPoolByItem(const std::vector<VehicleStationNode*>& pool)
    {
        // Preserve first-seen item order while accumulating.
        std::vector<PoolItemGroup> groups;
        auto slot = [&](const Item* it) -> PoolItemGroup& {
            for (auto& g : groups) if (g.item == it) return g;
            groups.push_back(PoolItemGroup{ it, {}, {} });
            return groups.back();
        };
        for (VehicleStationNode* s : pool)
        {
            if (s->mode == VehicleStationNode::Mode::Load)
            {
                // Cargo inputs feed supply; the fuel inlet is never cargo.
                for (const auto& p : s->ins)
                {
                    if (p->item != nullptr && !IsFuelPin(p.get())) slot(p->item).supply.push_back(p.get());
                }
            }
            else
            {
                for (const auto& p : s->outs)
                {
                    if (p->item != nullptr) slot(p->item).demand.push_back(p.get());
                }
            }
        }
        // Permissive: keep only items present on BOTH sides.
        std::vector<PoolItemGroup> out;
        for (auto& g : groups)
        {
            if (!g.supply.empty() && !g.demand.empty()) out.push_back(std::move(g));
        }
        return out;
    }

    void PropagateCargoItems(const std::vector<VehicleStationNode*>& pool)
    {
        // Distinct items currently loaded (supply) and unloaded (demand).
        std::vector<const Item*> supply_items;
        std::vector<const Item*> demand_items;
        auto add_unique = [](std::vector<const Item*>& v, const Item* it) {
            if (it == nullptr) return;
            for (const Item* e : v) if (e == it) return;
            v.push_back(it);
        };
        for (VehicleStationNode* s : pool)
        {
            if (s->mode == VehicleStationNode::Mode::Load)
            {
                for (const auto& p : s->ins) { if (!IsFuelPin(p.get())) add_unique(supply_items, p->item); }
            }
            else
            {
                for (const auto& p : s->outs) add_unique(demand_items, p->item);
            }
        }
        // Assign `it` to a station's active side: skip if already present, else
        // fill the first empty (non-fuel) slot. Never overwrites.
        auto fill = [](std::vector<std::unique_ptr<Pin>>& pins, const Item* it, bool skip_fuel) {
            Pin* empty = nullptr;
            for (auto& p : pins)
            {
                if (skip_fuel && IsFuelPin(p.get())) continue;
                if (p->item == it) return;
                if (p->item == nullptr && empty == nullptr) empty = p.get();
            }
            if (empty != nullptr) empty->item = it;
        };
        // A station's cargo belts all carry its item(s): once items are placed,
        // if the active side ends up with a single distinct item, mark every
        // remaining empty belt with it so both belts show the item (a second
        // item, if present, keeps its own belt).
        auto fill_uniform = [](std::vector<std::unique_ptr<Pin>>& pins, bool skip_fuel) {
            const Item* only = nullptr;
            for (auto& p : pins)
            {
                if (skip_fuel && IsFuelPin(p.get())) continue;
                if (p->item == nullptr) continue;
                if (only == nullptr) only = p->item;
                else if (only != p->item) return; // mixed items: leave as-is
            }
            if (only == nullptr) return; // nothing to mirror
            for (auto& p : pins)
            {
                if (skip_fuel && IsFuelPin(p.get())) continue;
                if (p->item == nullptr) p->item = only;
            }
        };
        for (VehicleStationNode* s : pool)
        {
            if (s->mode == VehicleStationNode::Mode::Load)
            {
                for (const Item* it : demand_items) fill(s->ins, it, true);
                fill_uniform(s->ins, true);
            }
            else
            {
                for (const Item* it : supply_items) fill(s->outs, it, false);
                fill_uniform(s->outs, false);
            }
        }
    }

    bool ResolveRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                          std::vector<std::unique_ptr<Link>>& links,
                          const std::vector<VehicleStationNode*>& pool,
                          float& error_time, float error_flow_duration)
    {
        // A pool can carry several distinct cargo items (e.g. a train dropping
        // Coal at one stop and Iron Ore at another). Each item flows on its own
        // pins, so seed and solve once per distinct item rather than returning
        // after the first — otherwise every item but the first stays unbalanced.
        bool ok = true;
        std::unordered_set<const Item*> solved;
        auto try_solve = [&](Pin* p) {
            if (!IsActiveCargoPin(p) || p->item == nullptr || p->current_rate.GetNumerator() == 0) return;
            if (!solved.insert(p->item).second) return; // already solved this item
            if (!RateSolver::Solve(nodes, links, p, p->current_rate, error_time, error_flow_duration))
                ok = false;
        };
        for (VehicleStationNode* s : pool)
        {
            for (const auto& p : s->ins) try_solve(p.get());
            for (const auto& p : s->outs) try_solve(p.get());
        }
        return ok;
    }

    bool SyncRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const std::vector<VehicleStationNode*>& pool,
                       float& error_time, float error_flow_duration)
    {
        PropagateCargoItems(pool);
        return ResolveRoutePool(nodes, links, pool, error_time, error_flow_duration);
    }
}
