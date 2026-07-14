#include "domain/snapshot/rate_propagation.hpp"

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"

namespace
{
    // The dedicated fuel inlet on a Truck/Train station is the last input pin and
    // is isolated from the cargo flow: it must not feed the station's outputs nor
    // contribute to the throughput rate. Mirrors IsStationFuelPin in sav_import.cpp
    // (and IsFuelPin in production_app.cpp).
    bool IsStationFuelPin(const Node* node, const Pin* pin)
    {
        if (node == nullptr || !node->IsLogistics() || node->ins.empty()) return false;
        const LogisticsNode* l = static_cast<const LogisticsNode*>(node);
        if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
            l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
        return pin == node->ins.back().get();
    }
}

void PropagateRates(const std::vector<std::unique_ptr<Node>>& nodes,
                    const std::vector<std::unique_ptr<Link>>& links,
                    const std::unordered_set<const Node*>& mixed_item_nodes)
{
    // Forward-propagate rates from producers (CraftNode/Extractor outputs have
    // current_rate set upstream) through the wired graph:
    //   - Merger output  = sum(inputs)
    //   - Splitter outs  = input / count(connected outputs)
    //   - Logistics outs = sum(inputs) / count(connected outputs)
    //   - Link propagation copies upstream output rate onto the downstream
    //     input pin, except for CraftNode/Extractor pins, whose rates come
    //     from the recipe and must not be overwritten.
    // Iterates to a fixed point so multi-hop chains converge.
    constexpr int kMaxRatePropagationIters = 64;
    for (int iter = 0; iter < kMaxRatePropagationIters; ++iter)
    {
        bool changed = false;

        for (const auto& node : nodes)
        {
            if (node->IsMerger())
            {
                FractionalNumber sum(0, 1);
                for (const auto& p : node->ins) sum = sum + p->current_rate;
                Pin* op = node->outs[0].get();
                if (op->current_rate != sum)
                {
                    op->current_rate = sum;
                    changed = true;
                }
            }
            else if (node->IsGameSplitter() || node->IsCustomSplitter())
            {
                size_t connected_outs = 0;
                for (const auto& p : node->outs)
                {
                    if (p->link != nullptr) connected_outs += 1;
                }
                if (connected_outs == 0) continue;
                const FractionalNumber per_out = node->ins[0]->current_rate
                    / FractionalNumber(static_cast<long long>(connected_outs));
                for (const auto& p : node->outs)
                {
                    if (p->link == nullptr) continue;
                    if (p->current_rate != per_out)
                    {
                        p->current_rate = per_out;
                        changed = true;
                    }
                }
            }
            else if (node->IsLogistics())
            {
                FractionalNumber sum(0, 1);
                const Item* in_item = nullptr;
                for (const auto& p : node->ins)
                {
                    if (IsStationFuelPin(node.get(), p.get())) continue;
                    sum = sum + p->current_rate;
                    if (in_item == nullptr) in_item = p->item;
                }
                size_t connected_outs = 0;
                for (const auto& p : node->outs)
                {
                    if (p->link != nullptr) connected_outs += 1;
                }
                if (connected_outs == 0) continue;
                const FractionalNumber per_out = sum
                    / FractionalNumber(static_cast<long long>(connected_outs));
                for (const auto& p : node->outs)
                {
                    if (p->link == nullptr) continue;
                    if (p->current_rate != per_out)
                    {
                        p->current_rate = per_out;
                        changed = true;
                    }
                    // Pass-through item: a storage / station's output carries
                    // whatever its inputs carry. Without this the last storage in
                    // a chain ends up displaying the item of whatever
                    // non-logistics node it happens to feed.
                    if (mixed_item_nodes.find(node.get()) == mixed_item_nodes.end()
                        && in_item != nullptr
                        && p->item != in_item)
                    {
                        p->item = in_item;
                        changed = true;
                    }
                }
            }
        }

        for (const auto& link : links)
        {
            if (link->start == nullptr || link->end == nullptr) continue;
            Pin* upstream = link->start;
            Pin* downstream = link->end;
            if (downstream->node->IsCraft() || downstream->node->IsExtractor()) continue;
            if (downstream->current_rate != upstream->current_rate)
            {
                downstream->current_rate = upstream->current_rate;
                changed = true;
            }
            // Item: only copy upstream onto downstream when the downstream is a
            // logistics input. Organizer pins are handled by their own ChangeItem
            // step earlier; CraftNode/Extractor/Sink keep their recipe/resource-
            // bound items.
            if (downstream->node->IsLogistics()
                && upstream->item != nullptr
                && downstream->item != upstream->item)
            {
                downstream->item = upstream->item;
                changed = true;
            }
        }

        if (!changed) break;
    }
}
