#include "domain/solver/rate_solver_internal.hpp"

#include "domain/graph/graph_item_resolve.hpp"   // IsActiveCargoPin
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/vehicle/vehicle_route.hpp"

#include <imgui_node_editor.h>

#include <algorithm>
#include <functional>
#include <queue>
#include <set>

#define DEBUG_PROPAGATION 0

#if DEBUG_PROPAGATION
static std::unordered_set<const Pin*> graph_update_pins;
static std::unordered_set<const Pin*> graph_update_multi_pins_constrained;
#endif

namespace
{
    // The far end of a belt pin's link (the pin feeding/consuming this one).
    Pin* BeltFarEnd(const Pin* pin)
    {
        if (pin->link == nullptr) return nullptr;
        return pin->direction == ax::NodeEditor::PinKind::Input ? pin->link->start : pin->link->end;
    }
}

namespace rate_solver_detail
{
    SeedResult SeedRelevantPins(const SolveInputs& in)
    {
        SeedResult seed;

        // Current queue of pins that had their new rate set and need to propagate it
        std::queue<const Pin*> pins_to_propagate;
        pins_to_propagate.push(in.constraint_pin);
        // We need to process the first link here to prevent infinite loop
        // in which each end triggers an update of the other one
        if (in.constraint_pin->link != nullptr)
        {
            pins_to_propagate.push(in.constraint_pin->direction == ax::NodeEditor::PinKind::Input ? in.constraint_pin->link->start : in.constraint_pin->link->end);
            if (!in.constraint_pin->link->flow.has_value())
            {
                in.constraint_pin->link->flow = in.constraint_pin->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
            }
        }

        // A secondary queue to store multi-pin (merger/custom splitter) that should be updated,
        // but could also be constrained, so wait to see and if not constrained before propagating from it
        std::queue<const Pin*> multi_pin_maybe_constrained;

        // Expand each (pool, item) group only once. Pool identity is its smallest
        // member pointer (stable across members of the same connected component).
        std::set<std::pair<const void*, const Item*>> processed_groups;

        // Collect all pins involved in this graph operation
        while (!pins_to_propagate.empty() || !multi_pin_maybe_constrained.empty())
        {
            while (pins_to_propagate.empty())
            {
                if (multi_pin_maybe_constrained.empty())
                {
                    break;
                }
                const Pin* maybe_pin = multi_pin_maybe_constrained.front();
                multi_pin_maybe_constrained.pop();
                // This pin doesn't have other external constraint, propagate from it
                if (seed.relevant_pins.find(maybe_pin) == seed.relevant_pins.end())
                {
                    seed.relevant_pins.insert(maybe_pin);
                    if (maybe_pin->link != nullptr)
                    {
                        const Pin* linked_pin = maybe_pin->direction == ax::NodeEditor::PinKind::Input ? maybe_pin->link->start : maybe_pin->link->end;
                        pins_to_propagate.push(linked_pin);
                        if (!maybe_pin->link->flow.has_value())
                        {
                            maybe_pin->link->flow = maybe_pin->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
                        }
                    }
                }
            }
            // No valid multi_pin_maybe_constrained, just break
            if (pins_to_propagate.empty())
            {
                break;
            }
            const Pin* updated_pin = pins_to_propagate.front();
            pins_to_propagate.pop();

            seed.relevant_pins.insert(updated_pin);

            switch (updated_pin->node->GetKind())
            {
            case Node::Kind::Craft:
            case Node::Kind::Group:
            case Node::Kind::GameSplitter:
            // Craft/Group/GameSplitter, any pin update triggers an update of all pins
            {
                for (const auto& p : updated_pin->node->ins)
                {
                    // Update the other inputs
                    if (p.get() != updated_pin && seed.relevant_pins.find(p.get()) == seed.relevant_pins.end())
                    {
                        seed.relevant_pins.insert(p.get());
                        if (p->link != nullptr)
                        {
                            pins_to_propagate.push(p->link->start);
                            if (!p->link->flow.has_value())
                            {
                                p->link->flow = p->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
                            }
                        }
                    }
                }
                for (const auto& p : updated_pin->node->outs)
                {
                    // Update the other outputs
                    if (p.get() != updated_pin && seed.relevant_pins.find(p.get()) == seed.relevant_pins.end())
                    {
                        seed.relevant_pins.insert(p.get());
                        if (p->link != nullptr)
                        {
                            pins_to_propagate.push(p->link->end);
                            if (!p->link->flow.has_value())
                            {
                                p->link->flow = p->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
                            }
                        }
                    }
                }
            }
            break;
            case Node::Kind::CustomSplitter:
            case Node::Kind::Merger:
                // It was an output pin updated by something external (user or link), add it to the strongly constrained pins
                if ((updated_pin->node->GetKind() == Node::Kind::CustomSplitter && updated_pin->direction == ax::NodeEditor::PinKind::Output) ||
                    (updated_pin->node->GetKind() == Node::Kind::Merger && updated_pin->direction == ax::NodeEditor::PinKind::Input)
                )
                {
                    seed.multi_pin_constrained.insert(updated_pin);
                }

                {
                    const Pin* single_pin = updated_pin->node->GetKind() == Node::Kind::CustomSplitter ? updated_pin->node->ins[0].get() : updated_pin->node->outs[0].get();
                    const std::vector<std::unique_ptr<Pin>>& multi_pin = updated_pin->node->GetKind() == Node::Kind::CustomSplitter ? updated_pin->node->outs : updated_pin->node->ins;
                    // Process the single pin
                    if (!single_pin->GetLocked() && single_pin != updated_pin && seed.relevant_pins.find(single_pin) == seed.relevant_pins.end())
                    {
                        seed.relevant_pins.insert(single_pin);
                        if (single_pin->link != nullptr)
                        {
                            pins_to_propagate.push(single_pin->direction == ax::NodeEditor::PinKind::Input ? single_pin->link->start : single_pin->link->end);
                            if (!single_pin->link->flow.has_value())
                            {
                                single_pin->link->flow = single_pin->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
                            }
                        }
                    }

                    // Process the other output pins
                    for (const auto& p : multi_pin)
                    {
                        if (!p->GetLocked() && updated_pin != p.get() && seed.relevant_pins.find(p.get()) == seed.relevant_pins.end())
                        {
                            // Don't propagate yet
                            multi_pin_maybe_constrained.push(p.get());
                        }
                    }
                }
                break;
            case Node::Kind::Sink:
            case Node::Kind::Extractor:
                // Sink/Extractor updates don't trigger other pins here.
                break;
            case Node::Kind::Logistics:
                // A vehicle station's active cargo pin pulls its whole route pool
                // into the solve: the matching (pool, item) group's supply and demand
                // pins become relevant, and each one's upstream/downstream belt is
                // propagated so the full chain settles together. Other logistics
                // nodes (Storage, ...) trigger nothing.
                if (IsActiveCargoPin(updated_pin))
                {
                    VehicleStationNode* station = static_cast<VehicleStationNode*>(updated_pin->node);
                    std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(station);
                    // Stable per-pool id (smallest member by total pointer order).
                    const void* pool_id = pool.empty() ? nullptr : static_cast<const void*>(
                        *std::min_element(pool.begin(), pool.end(),
                            [](const VehicleStationNode* a, const VehicleStationNode* b) {
                                return std::less<const void*>{}(a, b);
                            }));
                    const Item* item = updated_pin->item;
                    if (processed_groups.insert({ pool_id, item }).second)
                    {
                        for (const VehicleRoute::PoolItemGroup& g : VehicleRoute::GroupPoolByItem(pool))
                        {
                            if (g.item != item) continue;
                            ActiveGroup ag;
                            auto pull = [&](Pin* p) {
                                seed.relevant_pins.insert(p);
                                if (p->link != nullptr)
                                {
                                    pins_to_propagate.push(BeltFarEnd(p));
                                    if (!p->link->flow.has_value())
                                    {
                                        p->link->flow = p->direction == ax::NodeEditor::PinKind::Input ?
                                            ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
                                    }
                                }
                            };
                            // Only belts that actually carry flow take part in the
                            // balance: a belt is a participant when it is connected
                            // (has a belt link), locked, already carrying a rate, or
                            // is the very pin the user is editing. An idle, unconnected
                            // belt that merely shows the item type is skipped, so flow
                            // never gets split onto a belt nothing is attached to.
                            auto is_participant = [&](const Pin* p) {
                                return p->link != nullptr || p->GetLocked()
                                    || p->current_rate.GetNumerator() != 0 || p == in.constraint_pin;
                            };
                            for (Pin* p : g.supply) { if (is_participant(p)) { ag.supply.push_back(p); pull(p); } }
                            for (Pin* p : g.demand) { if (is_participant(p)) { ag.demand.push_back(p); pull(p); } }
                            // Permissive: a group needs a live pin on both sides.
                            if (!ag.supply.empty() && !ag.demand.empty()) seed.active_groups.push_back(std::move(ag));
                        }
                    }
                }
                break;
            }
        }

#if DEBUG_PROPAGATION
        graph_update_pins = seed.relevant_pins;
        graph_update_multi_pins_constrained = seed.multi_pin_constrained;
#endif

        return seed;
    }
}
