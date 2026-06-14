#include "domain/rate_solver.hpp"
#include "domain/building.hpp"
#include "domain/graph_item_resolve.hpp"
#include "domain/linear_solve.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/link.hpp"
#include "domain/recipe.hpp"
#include "domain/fractional_number.hpp"
#include "domain/vehicle_route.hpp"

#include <imgui_node_editor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <cstdio>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    // Clear per-pin error flags and link flow at the start of a solve.
    void ResetGraphState(std::vector<std::unique_ptr<Node>>& nodes,
                         std::vector<std::unique_ptr<Link>>& links,
                         float& error_time)
    {
        for (const auto& n : nodes)
        {
            for (const auto& p : n->ins)
            {
                p->error = false;
            }
            for (const auto& p : n->outs)
            {
                p->error = false;
            }
        }
        error_time = 0.0f;

        for (auto& l : links)
        {
            l->flow = std::nullopt;
        }
    }
}

bool RateSolver::Solve(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const Pin* constraint_pin,
                       const FractionalNumber& constraint_value,
                       float& error_time,
                       float error_flow_duration)
{
#if DEBUG_PROPAGATION
    fprintf(stderr, "================================= BEGIN PROPAGATION =================================\n");
    struct OnExit { ~OnExit() { fprintf(stderr, "================================= END PROPAGATION =================================\n"); } } _on_exit;
#endif

    // Reset per-pin errors and link flow.
    ResetGraphState(nodes, links, error_time);

    // Will store all pins impacted by this graph update
    std::unordered_set<const Pin*> relevant_pins;
    // Will store all pins on the multi-pin side of a CustomSplitter/Merger
    // that are impacted by this graph update with a "strong" constraint
    // (like a being the start/end of a link connected to another updated pin)
    // Pins updated as "potential overflow from another multi pin update" have "weak" constraints
    std::unordered_set<const Pin*> multi_pin_constrained;

    // Current queue of pins that had their new rate set and need to propagate it
    std::queue<const Pin*> pins_to_propagate;
    pins_to_propagate.push(constraint_pin);
    // We need to process the first link here to prevent infinite loop
    // in which each end triggers an update of the other one
    if (constraint_pin->link != nullptr)
    {
        pins_to_propagate.push(constraint_pin->direction == ax::NodeEditor::PinKind::Input ? constraint_pin->link->start : constraint_pin->link->end);
        if (!constraint_pin->link->flow.has_value())
        {
            constraint_pin->link->flow = constraint_pin->direction == ax::NodeEditor::PinKind::Input ? ax::NodeEditor::FlowDirection::Backward : ax::NodeEditor::FlowDirection::Forward;
        }
    }

    // A secondary queue to store multi-pin (merger/custom splitter) that should be updated,
    // but could also be constrained, so wait to see and if not constrained before propagating from it
    std::queue<const Pin*> multi_pin_maybe_constrained;

    // Vehicle route pools reached by this solve. Each active (pool, item) group
    // behaves like a merger (supply -> T) feeding a splitter (T -> demand): its
    // cargo pins are tied together through an auxiliary "total" variable T added
    // after all pin variables. Tracked here, materialised into equations below.
    struct ActiveGroup
    {
        std::vector<Pin*> supply;
        std::vector<Pin*> demand;
    };
    std::vector<ActiveGroup> active_groups;
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
            if (relevant_pins.find(maybe_pin) == relevant_pins.end())
            {
                relevant_pins.insert(maybe_pin);
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

        relevant_pins.insert(updated_pin);

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
                if (p.get() != updated_pin && relevant_pins.find(p.get()) == relevant_pins.end())
                {
                    relevant_pins.insert(p.get());
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
                if (p.get() != updated_pin && relevant_pins.find(p.get()) == relevant_pins.end())
                {
                    relevant_pins.insert(p.get());
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
                multi_pin_constrained.insert(updated_pin);
            }

            {
                const Pin* single_pin = updated_pin->node->GetKind() == Node::Kind::CustomSplitter ? updated_pin->node->ins[0].get() : updated_pin->node->outs[0].get();
                const std::vector<std::unique_ptr<Pin>>& multi_pin = updated_pin->node->GetKind() == Node::Kind::CustomSplitter ? updated_pin->node->outs : updated_pin->node->ins;
                // Process the single pin
                if (!single_pin->GetLocked() && single_pin != updated_pin && relevant_pins.find(single_pin) == relevant_pins.end())
                {
                    relevant_pins.insert(single_pin);
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
                    if (!p->GetLocked() && updated_pin != p.get() && relevant_pins.find(p.get()) == relevant_pins.end())
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
                            relevant_pins.insert(p);
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
                                || p->current_rate.GetNumerator() != 0 || p == constraint_pin;
                        };
                        for (Pin* p : g.supply) { if (is_participant(p)) { ag.supply.push_back(p); pull(p); } }
                        for (Pin* p : g.demand) { if (is_participant(p)) { ag.demand.push_back(p); pull(p); } }
                        // Permissive: a group needs a live pin on both sides.
                        if (!ag.supply.empty() && !ag.demand.empty()) active_groups.push_back(std::move(ag));
                    }
                }
            }
            break;
        }
    }

#if DEBUG_PROPAGATION
    graph_update_pins = relevant_pins;
    graph_update_multi_pins_constrained = multi_pin_constrained;
#endif

    // For each updated pin, the associated variable with a ratio
    // The ratio allows us to use only one variable/equation per craft node
    std::unordered_map<const Pin*, std::pair<size_t, FractionalNumber>> associated_variable_index;
    size_t num_variables = 0;

    // Lambda function to create a variable for a pin
    // For nodes sharing one variable for all pins, will link the variable to all pins
    auto create_variable = [&](const Pin* pin) {
        // Pin already associated to a variable
        if (associated_variable_index.find(pin) != associated_variable_index.end())
        {
            return;
        }
        // No variable yet for this pin, create a new one
        switch (pin->node->GetKind())
        {
        // Craft/Group/GameSplitter, only one variable for all the pins
        case Node::Kind::Craft:
        case Node::Kind::Group:
        case Node::Kind::GameSplitter:
            for (auto& p : pin->node->ins)
            {
                // For craft/group, the ratio is the base rate of the pin
                if (pin->node->IsPowered())
                {
                    associated_variable_index.insert({ p.get(), { num_variables, p->base_rate } });
                }
                // For GameSplitter, the variable is the rate of the input
                else if (pin->node->IsGameSplitter())
                {
                    associated_variable_index.insert({ p.get(), { num_variables, 1 } });
                }
            }
            for (auto& p : pin->node->outs)
            {
                // For group, the ratio is the base rate of the pin
                if (pin->node->IsGroup())
                {
                    associated_variable_index.insert({ p.get(), { num_variables, p->base_rate } });
                }
                // For craft, we need to deal with somersloop
                else if (pin->node->IsCraft())
                {
                    const CraftNode* craft_node = static_cast<CraftNode*>(p->node);
                    associated_variable_index.insert({ p.get(), { num_variables, p->base_rate * (1 + (craft_node->num_somersloop * craft_node->recipe->building->somersloop_mult)) } });
                }
                // For GameSplitter, the variable is the rate of the input divided by the number of outs
                else if (pin->node->IsGameSplitter())
                {
                    associated_variable_index.insert({ p.get(), { num_variables, FractionalNumber(1, pin->node->outs.size()) } });
                }
            }
            break;
        // Extractor: only outputs, one variable for the node. Rate ratio is
        // the live base × purity (mirrors how ExtractorNode::UpdateRate works).
        case Node::Kind::Extractor:
            for (auto& p : pin->node->outs)
            {
                const ExtractorNode* ex = static_cast<const ExtractorNode*>(p->node);
                associated_variable_index.insert({ p.get(), { num_variables, ex->GetBaseRate() * ex->GetPurityMultiplier() } });
            }
            break;
        // For Merger, CustomSplitter and Sink, we need one variable per pin
        case Node::Kind::CustomSplitter:
        case Node::Kind::Merger:
        case Node::Kind::Sink:
        case Node::Kind::Logistics:
            associated_variable_index.insert({ pin, { num_variables, 1 } });
            break;
        }
        // Increase the variable index
        num_variables += 1;
    };

    // Create first the variables related to strongly constrained pins (all but the overflow/secondary pins of merger/customsplitter)
    for (const Pin* pin : relevant_pins)
    {
        switch (pin->node->GetKind())
        {
        case Node::Kind::CustomSplitter:
            if (pin->direction == ax::NodeEditor::PinKind::Input || multi_pin_constrained.find(pin) != multi_pin_constrained.end())
            {
                create_variable(pin);
            }
            break;
        case Node::Kind::Merger:
            if (pin->direction == ax::NodeEditor::PinKind::Output || multi_pin_constrained.find(pin) != multi_pin_constrained.end())
            {
                create_variable(pin);
            }
            break;
        case Node::Kind::Craft:
        case Node::Kind::Group:
        case Node::Kind::GameSplitter:
        case Node::Kind::Sink:
        case Node::Kind::Extractor:
        case Node::Kind::Logistics:
            create_variable(pin);
            break;
        }
    }

    // Then create variables for the remaining pins (this way they will be the last variables and used later as pivot when solving the equations)
    for (const Pin* pin : relevant_pins)
    {
        create_variable(pin);
    }

    // Allocate one auxiliary "total" variable T per active route group, after
    // every pin variable (so existing pivot ordering is untouched). T maps to no
    // pin: it is skipped by the negative-rate check and rate-assignment loops,
    // and reversed_variable_map[T] stays null.
    std::vector<size_t> group_total_index(active_groups.size());
    // For the rare case T itself is a free variable (a totally underdetermined
    // group), fall back to the group's current supply total to avoid touching a
    // null pin in the free-variable resolver.
    std::unordered_map<size_t, FractionalNumber> group_total_default;
    for (size_t gi = 0; gi < active_groups.size(); ++gi)
    {
        group_total_index[gi] = num_variables;
        FractionalNumber supply_total(0, 1);
        for (const Pin* p : active_groups[gi].supply) supply_total += p->current_rate;
        group_total_default[num_variables] = supply_total;
        num_variables += 1;
    }
    // Reverse lookup: which group/side a pin belongs to (for ratio resolution).
    std::unordered_map<const Pin*, std::pair<size_t, bool>> group_membership; // bool: is_supply
    for (size_t gi = 0; gi < active_groups.size(); ++gi)
    {
        for (const Pin* p : active_groups[gi].supply) group_membership[p] = { gi, true };
        for (const Pin* p : active_groups[gi].demand) group_membership[p] = { gi, false };
    }

    // Map a variable index to a pin pointer (for variables linked to multiple pins, just select one of them)
    std::vector<const Pin*> reversed_variable_map(num_variables, nullptr);
    for (const auto& [p, pp] : associated_variable_index)
    {
        reversed_variable_map[pp.first] = p;
    }

    // Now we can create the main equations linked to this graph operation
    // The end goal is to solve equations_coefficients * x = constants
    std::vector<std::vector<FractionalNumber>> equations_coefficients;
    std::vector<FractionalNumber> constants;
    // Used to prevent double processing the same link (it would work with duplicated equations but more efficient to avoid creating them in the first place)
    std::unordered_set<const Link*> processed_links;
    // used to prevent double processing the same Merger/CustomSplitter node
    std::unordered_set<const Node*> processed_multi_node;

    // First equation is from the user input
    // rate * user variable = user constraint
    {
        std::vector<FractionalNumber> equation(num_variables);
        const auto& variable = associated_variable_index.at(constraint_pin);
        equation[variable.first] = variable.second;
        equations_coefficients.push_back(equation);
        constants.push_back(constraint_value);
    }

    // Then process all pins to add an equality equation per link and a balance equation for all merger/customsplitter
    for (const Pin* pin : relevant_pins)
    {
        if (pin->link != nullptr && processed_links.find(pin->link) == processed_links.end())
        {
            // Add an equation for equality
            // X - Y = 0
            std::vector<FractionalNumber> equation(num_variables);
            const std::pair<size_t, FractionalNumber>& start_variable = associated_variable_index.at(pin->link->start);
            const std::pair<size_t, FractionalNumber>& end_variable = associated_variable_index.at(pin->link->end);
            equation[start_variable.first] = start_variable.second;
            equation[end_variable.first] = -1 * end_variable.second;
            equations_coefficients.push_back(equation);
            constants.push_back(0);
            processed_links.insert(pin->link);
        }

        if ((pin->node->GetKind() == Node::Kind::Merger || pin->node->GetKind() == Node::Kind::CustomSplitter) && processed_multi_node.find(pin->node) == processed_multi_node.end())
        {
            // Add an equation sum(inputs) - sum(outputs) = 0
            std::vector<FractionalNumber> equation(num_variables);
            FractionalNumber constant = 0;

            for (const std::unique_ptr<Pin>& i : pin->node->ins)
            {
                if (i->GetLocked())
                {
                    constant -= i->current_rate;
                }
                else
                {
                    equation[associated_variable_index.at(i.get()).first] = 1;
                }
            }

            for (const std::unique_ptr<Pin>& o : pin->node->outs)
            {
                if (o->GetLocked())
                {
                    constant += o->current_rate;
                }
                else
                {
                    equation[associated_variable_index.at(o.get()).first] = -1;
                }
            }
            equations_coefficients.push_back(equation);
            constants.push_back(constant);
            processed_multi_node.insert(pin->node);
        }
    }

    // Per route-group balance: each group behaves as merger(supply -> T) feeding
    // splitter(T -> demand). Locked pins fold into the constant exactly as the
    // merger/splitter balance does above.
    //   sum(unlocked supply) - T = -sum(locked supply)
    //   T - sum(unlocked demand) =  sum(locked demand)
    for (size_t gi = 0; gi < active_groups.size(); ++gi)
    {
        const ActiveGroup& g = active_groups[gi];
        const size_t T = group_total_index[gi];
        {
            std::vector<FractionalNumber> equation(num_variables);
            FractionalNumber constant = 0;
            for (Pin* p : g.supply)
            {
                if (p->GetLocked()) constant -= p->current_rate;
                else equation[associated_variable_index.at(p).first] += 1;
            }
            equation[T] = -1;
            equations_coefficients.push_back(equation);
            constants.push_back(constant);
        }
        {
            std::vector<FractionalNumber> equation(num_variables);
            FractionalNumber constant = 0;
            equation[T] = 1;
            for (Pin* p : g.demand)
            {
                if (p->GetLocked()) constant += p->current_rate;
                else equation[associated_variable_index.at(p).first] += -1;
            }
            equations_coefficients.push_back(equation);
            constants.push_back(constant);
        }
    }

    // Reduce the equation matrix using gaussian elimination (pure numeric
    // kernel extracted to domain/linear_solve).
    std::vector<std::vector<FractionalNumber>> reduced_matrix =
        ReduceMatrix(equations_coefficients, constants, num_variables);

    // We had more equations than variables, the only way we have a solution
    // is if all last lines are full 0 otherwise there is no solution
    if (num_variables < equations_coefficients.size())
    {
        for (size_t i = num_variables; i < equations_coefficients.size(); ++i)
        {
            for (size_t j = 0; j < num_variables + 1; ++j)
            {
                if (reduced_matrix[i][j] != 0)
                {
                    error_time = error_flow_duration;
                    return false;
                }
            }
        }
    }

    // An inconsistent row (all coefficients zero, constant non-zero) means the
    // system has no solution regardless of free variables. This can happen with
    // exactly as many equations as variables (e.g. an over-constrained route
    // whose locked supply total can't match its locked demand total), where the
    // check above doesn't apply. Catch it before the free-variable loop, which
    // would otherwise spin trying to resolve a remaining free variable.
    for (size_t i = 0; i < reduced_matrix.size(); ++i)
    {
        bool all_coeffs_zero = true;
        for (size_t j = 0; j < num_variables; ++j)
        {
            if (reduced_matrix[i][j] != 0) { all_coeffs_zero = false; break; }
        }
        if (all_coeffs_zero && reduced_matrix[i][num_variables] != 0)
        {
            error_time = error_flow_duration;
            return false;
        }
    }

    std::unordered_set<const Pin*> processed_pins;
    // Loop until we resolved all free variables
    while (true)
    {
        std::unordered_set<int> free_variables;
        for (int i = 0; i < num_variables; ++i)
        {
            free_variables.insert(i);
        }
        for (int i = 0; i < equations_coefficients.size(); ++i)
        {
            int j = 0;
            while (j < num_variables && reduced_matrix[i][j].GetNumerator() == 0)
            {
                j += 1;
            }
            if (j < num_variables)
            {
                free_variables.erase(j);
            }
        }
        if (free_variables.empty())
        {
            break;
        }

#if DEBUG_PROPAGATION
        fprintf(stderr, "\nfree variables\n");
        for (const auto i : free_variables)
        {
            fprintf(stderr, "p%zu\n", static_cast<int>(reversed_variable_map[i]->id.Get()));
        }
#endif

        const size_t previous_num_equations = equations_coefficients.size();
        // Process the first free variable
        const int free_index = *free_variables.begin();
        const Pin* pin = reversed_variable_map[free_index];

        // If we haven't met this free pin yet
        if (processed_pins.find(pin) == processed_pins.end())
        {
            const auto group_it = (pin == nullptr) ? group_membership.end() : group_membership.find(pin);
            if (pin == nullptr)
            {
                // A group total T has no driving constraint (a fully
                // underdetermined group): fix it to the group's current supply
                // total so resolution proceeds without touching a null pin.
                equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                equations_coefficients.back()[free_index] = 1;
                const auto default_it = group_total_default.find(free_index);
                constants.push_back(default_it != group_total_default.end() ? default_it->second : FractionalNumber(0, 1));
            }
            else if (group_it != group_membership.end())
            {
                // §6: this pin is on one side of a route group. Keep every
                // unlocked pin on that side at its prior share of the group total
                // T (supply behaves like a merger's inputs, demand like a
                // splitter's outputs, both relative to T which is never locked).
                const size_t gi = group_it->second.first;
                const bool is_supply = group_it->second.second;
                const size_t T = group_total_index[gi];
                const std::vector<Pin*>& side = is_supply ? active_groups[gi].supply : active_groups[gi].demand;
                FractionalNumber old_sum_not_locked;
                FractionalNumber sum_locked;
                size_t num_unlocked = 0;
                for (Pin* p : side)
                {
                    if (!p->GetLocked()) { old_sum_not_locked += p->current_rate; num_unlocked += 1; }
                    else { sum_locked += p->current_rate; }
                }
                if (num_unlocked == 0)
                {
                    equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                    equations_coefficients.back()[free_index] = 1;
                    constants.push_back(pin->current_rate);
                }
                else
                {
                    // Unlocked pins share (T - sum_locked) in their prior ratio.
                    // T is never locked, so this mirrors the merger/splitter ratio
                    // equation with single_pin -> T:
                    //   x_p - multiplier*T = -multiplier*sum_locked
                    for (Pin* p : side)
                    {
                        if (p->GetLocked()) continue;
                        const FractionalNumber multiplier =
                            old_sum_not_locked == 0 ?
                            FractionalNumber(1, num_unlocked) :
                            p->current_rate / old_sum_not_locked;
                        equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                        equations_coefficients.back()[associated_variable_index.at(p).first] = 1;
                        equations_coefficients.back()[T] = -1 * multiplier;
                        constants.push_back(-1 * multiplier * sum_locked);
                    }
                }
            }
            else
            {
            switch (pin->node->GetKind())
            {
                // I don't think it can happen as we set the variables for the merger/customsplitter at the end but just in case
            case Node::Kind::Craft:
            case Node::Kind::Group:
            case Node::Kind::GameSplitter:
            case Node::Kind::Sink:
            case Node::Kind::Extractor:
            case Node::Kind::Logistics:
                // Add an equation so that the pin keeps the same rate it currently has
                equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                equations_coefficients.back()[free_index] = 1;
                constants.push_back(pin->current_rate);
                break;
            case Node::Kind::CustomSplitter:
            case Node::Kind::Merger:
            {
                const Pin* single_pin = pin->node->GetKind() == Node::Kind::CustomSplitter ? pin->node->ins[0].get() : pin->node->outs[0].get();
                const std::vector<std::unique_ptr<Pin>>& multi_pin = pin->node->GetKind() == Node::Kind::CustomSplitter ? pin->node->outs : pin->node->ins;

                std::vector<size_t> already_constrained;
                FractionalNumber old_sum_not_constrained;
                FractionalNumber sum_locked;
                size_t num_unlocked_not_constraint = 0;
                for (const auto& p : multi_pin)
                {
                    if (multi_pin_constrained.find(p.get()) != multi_pin_constrained.end())
                    {
                        already_constrained.push_back(associated_variable_index.at(p.get()).first);
                    }
                    else if (!p->GetLocked())
                    {
                        old_sum_not_constrained += p->current_rate;
                        num_unlocked_not_constraint += 1;
                    }
                    else
                    {
                        sum_locked += p->current_rate;
                    }
                }

                // I'm not sure this can happen for a free variable but just in case
                if (num_unlocked_not_constraint == 0)
                {
                    // Add an equation so that the pin keeps the same rate it currently has
                    equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                    equations_coefficients.back()[free_index] = 1;
                    constants.push_back(pin->current_rate);
                }
                else
                {
                    // Add an equation to say that each multi pin of this node must keep the same ratio as it had before the update
                    // We don't add the condition only for this free pin as otherwise it could cause unbalanced update for the node
                    // This may end up overconstraining the system though need to test it more to be sure
                    for (const auto& p : multi_pin)
                    {
                        if (p->GetLocked())
                        {
                            continue;
                        }
                        if (multi_pin_constrained.find(p.get()) != multi_pin_constrained.end())
                        {
                            continue;
                        }
                        equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
                        const FractionalNumber multiplier =
                            old_sum_not_constrained == 0 ?
                            // If old sum was 0, just split evenly: new_rate - (single_pin - constrained) / num unconstrained = locked
                            FractionalNumber(1, num_unlocked_not_constraint) :
                            // Else, keep ratio: new_rate - old_rate / sum(old_rates) * (single_pin - constrained) = locked
                            p->current_rate / old_sum_not_constrained;
                        for (const auto i : already_constrained)
                        {
                            equations_coefficients.back()[i] = multiplier;
                        }
                        equations_coefficients.back()[associated_variable_index.at(p.get()).first] = 1;
                        if (single_pin->GetLocked())
                        {
                            constants.push_back(multiplier * (single_pin->current_rate - sum_locked));
                        }
                        else
                        {
                            equations_coefficients.back()[associated_variable_index.at(single_pin).first] = -1 * multiplier;
                            constants.push_back(-1 * multiplier * sum_locked);
                        }
                    }
                }
            }
            break;
            }
            }
        }
        // The ratio constraint wasn't enough, add a "keep current value" equation
        else
        {
            equations_coefficients.push_back(std::vector<FractionalNumber>(num_variables));
            equations_coefficients.back()[free_index] = 1;
            if (pin == nullptr)
            {
                const auto default_it = group_total_default.find(free_index);
                constants.push_back(default_it != group_total_default.end() ? default_it->second : FractionalNumber(0, 1));
            }
            else
            {
                constants.push_back(pin->current_rate);
            }
        }
        processed_pins.insert(pin);
        reduced_matrix = ReduceMatrix(equations_coefficients, constants, num_variables);

        // We added more equations than variables, the only way we have a solution
        // is if all last lines are full 0 otherwise there is no solution
        if (num_variables < equations_coefficients.size())
        {
            bool solution_exists = true;
            for (size_t i = num_variables; i < equations_coefficients.size(); ++i)
            {
                for (size_t j = 0; j < num_variables + 1; ++j)
                {
                    if (reduced_matrix[i][j] != 0)
                    {
                        solution_exists = false;
                        break;
                    }
                }
                if (!solution_exists)
                {
                    break;
                }
            }

            // If we added too many equations and the system no longer has a solution remove the
            // previously added equations so the next iteration adds just equality ones instead
            if (!solution_exists)
            {
                equations_coefficients.resize(previous_num_equations);
                constants.resize(previous_num_equations);
                reduced_matrix = ReduceMatrix(equations_coefficients, constants, num_variables);
            }
        }
    }


    // Back substitution
    std::vector<FractionalNumber> solution(num_variables);
    for (int i = num_variables - 1; i >= 0; --i)
    {
        FractionalNumber sum = 0;
        for (int j = i + 1; j < num_variables; ++j)
        {
            sum += reduced_matrix[i][j] * solution[j];
        }
        if (reduced_matrix[i][i].GetNumerator() == 0)
        {
            error_time = error_flow_duration;
            return false;
        }
        solution[i] = (reduced_matrix[i][num_variables] - sum) / reduced_matrix[i][i];
    }

    // Check for negative solution
    for (const auto& n : nodes)
    {
        for (const auto& p : n->ins)
        {
            const auto it = associated_variable_index.find(p.get());
            if (it != associated_variable_index.end() && solution[it->second.first] < 0)
            {
                p->error = true;
                error_time = error_flow_duration;
            }
        }
        for (const auto& p : n->outs)
        {
            const auto it = associated_variable_index.find(p.get());
            if (it != associated_variable_index.end() && solution[it->second.first] < 0)
            {
                p->error = true;
                error_time = error_flow_duration;
            }
        }
    }
    if (error_time > 0.0f)
    {
        return false;
    }

    // For each link check if flow should be kept
    for (auto& l : links)
    {
        const auto it_start = associated_variable_index.find(l->start);
        const auto it_end = associated_variable_index.find(l->end);
        // Both ends kept their value, don't flow
        if (it_start != associated_variable_index.end() && l->start->current_rate == solution[it_start->second.first] * it_start->second.second &&
            it_end != associated_variable_index.end() && l->end->current_rate == solution[it_end->second.first] * it_end->second.second)
        {
            l->flow = std::nullopt;
        }
    }

    // Set the updated new rates for each pin (and node)
    for (const auto& n : nodes)
    {
        for (const auto& p : n->ins)
        {
            const auto it = associated_variable_index.find(p.get());
            if (it != associated_variable_index.end())
            {
                const FractionalNumber new_rate = solution[it->second.first] * it->second.second;
                p->current_rate = new_rate;
                if (n->IsPowered())
                {
                    const FractionalNumber new_node_rate = new_rate / p->base_rate;
                    PoweredNode* powered_node = static_cast<PoweredNode*>(n.get());
                    if (new_node_rate != powered_node->current_rate)
                    {
                        powered_node->UpdateRate(new_node_rate);
                    }
                }
            }
        }
        for (const auto& p : n->outs)
        {
            const auto it = associated_variable_index.find(p.get());
            if (it != associated_variable_index.end())
            {
                const FractionalNumber new_rate = solution[it->second.first] * it->second.second;
                p->current_rate = new_rate;
                // Somersloop thing
                if (n->IsCraft())
                {
                    CraftNode* craft_node = static_cast<CraftNode*>(n.get());
                    const FractionalNumber new_node_rate = new_rate / (p->base_rate * (1 + craft_node->num_somersloop * craft_node->recipe->building->somersloop_mult));
                    if (new_node_rate != craft_node->current_rate)
                    {
                        craft_node->UpdateRate(new_node_rate);
                    }
                }
                else if (n->IsGroup())
                {
                    const FractionalNumber new_node_rate = new_rate / p->base_rate;
                    PoweredNode* powered_node = static_cast<PoweredNode*>(n.get());
                    if (new_node_rate != powered_node->current_rate)
                    {
                        powered_node->UpdateRate(new_node_rate);
                    }
                }
                else if (n->IsExtractor())
                {
                    ExtractorNode* ex = static_cast<ExtractorNode*>(n.get());
                    const FractionalNumber denom = ex->GetBaseRate() * ex->GetPurityMultiplier();
                    if (denom.GetNumerator() != 0)
                    {
                        const FractionalNumber new_node_rate = new_rate / denom;
                        if (new_node_rate != ex->current_rate)
                        {
                            ex->UpdateRate(new_node_rate);
                        }
                    }
                }
            }
        }
    }

    return true;
}
