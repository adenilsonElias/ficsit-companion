#define DEBUG_PROPAGATION 0

#include "domain/solver/rate_solver_internal.hpp"
#include "domain/solver/linear_solve.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/graph/link.hpp"
#include "domain/gamedata/recipe.hpp"
#include "domain/gamedata/building.hpp"

#include <cstdio>
#include <unordered_set>
#include <vector>

namespace rate_solver_detail
{

bool ApplyResults(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars,
                  LinearSystem& sys, ReducedSystem& reduced,
                  float& error_time, float error_flow_duration)
{
    // Keyed by variable index, not by pin: link variables have no pin, so keying by pin would
    // collide every one of them on the same null key.
    std::unordered_set<int> processed_variables;
    // Loop until we resolved all free variables
    while (true)
    {
        std::unordered_set<int> free_variables;
        for (int i = 0; i < vars.num_variables; ++i)
        {
            free_variables.insert(i);
        }
        for (int i = 0; i < sys.equations_coefficients.size(); ++i)
        {
            int j = 0;
            while (j < vars.num_variables && reduced.reduced_matrix[i][j].GetNumerator() == 0)
            {
                j += 1;
            }
            if (j < vars.num_variables)
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
            fprintf(stderr, "p%zu\n", static_cast<int>(vars.reversed_variable_map[i]->id.Get()));
        }
#endif

        const size_t previous_num_equations = sys.equations_coefficients.size();
        // Process the first free variable
        const int free_index = *free_variables.begin();
        const Pin* pin = vars.reversed_variable_map[free_index];

        // If we haven't met this free variable yet
        if (processed_variables.find(free_index) == processed_variables.end())
        {
            const auto link_it = vars.variable_of_link.find(free_index);
            const auto group_it = (pin == nullptr) ? vars.group_membership.end() : vars.group_membership.find(pin);
            if (link_it != vars.variable_of_link.end())
            {
                const Link* free_link = link_it->second;
                // Which pin owns the split this edge belongs to: the end that fans out. When both
                // ends fan out, the output side arbitrates - it is the one dividing a supply.
                const Pin* owner = nullptr;
                if (free_link->start != nullptr && free_link->start->links.size() > 1) owner = free_link->start;
                else if (free_link->end != nullptr && free_link->end->links.size() > 1) owner = free_link->end;

                if (owner == nullptr)
                {
                    // A lone edge with nothing driving it: keep the rate it already carried.
                    sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                    sys.equations_coefficients.back()[free_index] = 1;
                    sys.constants.push_back(free_link->current_rate);
                }
                else
                {
                    // The owner pin's total is pinned by its own balance equation, but how that
                    // total divides across the branches is free. Keep each branch at its prior
                    // share, which is the same rule a CustomSplitter uses for its output pins.
                    //
                    // A branch whose far pin is locked is fixed: it folds into the constant, and
                    // the unlocked branches share whatever is left.
                    //   x_l - multiplier * rate(owner) = -multiplier * sum_locked
                    // where rate(owner) is coef * var(owner), since a craft node's pins share one
                    // variable scaled by the recipe quantity.
                    auto far_pin = [owner](const Link* l) {
                        return l->start == owner ? l->end : l->start;
                    };
                    auto branch_locked = [&](const Link* l) {
                        const Pin* far = far_pin(l);
                        return far != nullptr && far->GetLocked();
                    };
                    // A branch whose far pin is the very pin the user is driving is already
                    // spoken for: its rate is dictated from outside, so it must not be dragged
                    // back to its prior share. This is what makes "wire up a new consumer" work -
                    // there the new branch is the constraint, and the old branches absorb nothing.
                    auto branch_constrained = [&](const Link* l) {
                        return far_pin(l) == in.constraint_pin;
                    };

                    FractionalNumber old_sum_free(0, 1);
                    FractionalNumber sum_locked(0, 1);
                    size_t num_free = 0;
                    std::vector<const Link*> constrained;
                    for (const Link* l : owner->links)
                    {
                        if (branch_constrained(l)) { constrained.push_back(l); }
                        else if (branch_locked(l)) { sum_locked += l->current_rate; }
                        else { old_sum_free += l->current_rate; num_free += 1; }
                    }

                    if (num_free == 0)
                    {
                        sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                        sys.equations_coefficients.back()[free_index] = 1;
                        sys.constants.push_back(free_link->current_rate);
                    }
                    else
                    {
                        const std::pair<size_t, FractionalNumber>& owner_variable =
                            vars.associated_variable_index.at(owner);
                        // Constrain every free branch at once, not just this one: pinning a single
                        // branch would leave the split unbalanced. Each takes its prior share of
                        // what is left after the locked and externally-driven branches.
                        //   x_l + m * sum(constrained) - m * rate(owner) = -m * sum_locked
                        for (const Link* l : owner->links)
                        {
                            if (branch_locked(l) || branch_constrained(l))
                            {
                                continue;
                            }
                            const FractionalNumber multiplier =
                                old_sum_free == 0 ?
                                FractionalNumber(1, num_free) :
                                l->current_rate / old_sum_free;
                            sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                            sys.equations_coefficients.back()[vars.link_variable_index.at(l)] = 1;
                            for (const Link* c : constrained)
                            {
                                sys.equations_coefficients.back()[vars.link_variable_index.at(c)] = multiplier;
                            }
                            sys.equations_coefficients.back()[owner_variable.first] =
                                -1 * multiplier * owner_variable.second;
                            sys.constants.push_back(-1 * multiplier * sum_locked);
                        }
                    }
                }
            }
            else if (pin == nullptr)
            {
                // A group total T has no driving constraint (a fully
                // underdetermined group): fix it to the group's current supply
                // total so resolution proceeds without touching a null pin.
                sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                sys.equations_coefficients.back()[free_index] = 1;
                const auto default_it = vars.group_total_default.find(free_index);
                sys.constants.push_back(default_it != vars.group_total_default.end() ? default_it->second : FractionalNumber(0, 1));
            }
            else if (group_it != vars.group_membership.end())
            {
                // §6: this pin is on one side of a route group. Keep every
                // unlocked pin on that side at its prior share of the group total
                // T (supply behaves like a merger's inputs, demand like a
                // splitter's outputs, both relative to T which is never locked).
                const size_t gi = group_it->second.first;
                const bool is_supply = group_it->second.second;
                const size_t T = vars.group_total_index[gi];
                const std::vector<Pin*>& side = is_supply ? seed.active_groups[gi].supply : seed.active_groups[gi].demand;
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
                    sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                    sys.equations_coefficients.back()[free_index] = 1;
                    sys.constants.push_back(pin->current_rate);
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
                        sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                        sys.equations_coefficients.back()[vars.associated_variable_index.at(p).first] = 1;
                        sys.equations_coefficients.back()[T] = -1 * multiplier;
                        sys.constants.push_back(-1 * multiplier * sum_locked);
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
                sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                sys.equations_coefficients.back()[free_index] = 1;
                sys.constants.push_back(pin->current_rate);
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
                    if (seed.multi_pin_constrained.find(p.get()) != seed.multi_pin_constrained.end())
                    {
                        already_constrained.push_back(vars.associated_variable_index.at(p.get()).first);
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
                    sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                    sys.equations_coefficients.back()[free_index] = 1;
                    sys.constants.push_back(pin->current_rate);
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
                        if (seed.multi_pin_constrained.find(p.get()) != seed.multi_pin_constrained.end())
                        {
                            continue;
                        }
                        sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
                        const FractionalNumber multiplier =
                            old_sum_not_constrained == 0 ?
                            // If old sum was 0, just split evenly: new_rate - (single_pin - constrained) / num unconstrained = locked
                            FractionalNumber(1, num_unlocked_not_constraint) :
                            // Else, keep ratio: new_rate - old_rate / sum(old_rates) * (single_pin - constrained) = locked
                            p->current_rate / old_sum_not_constrained;
                        for (const auto i : already_constrained)
                        {
                            sys.equations_coefficients.back()[i] = multiplier;
                        }
                        sys.equations_coefficients.back()[vars.associated_variable_index.at(p.get()).first] = 1;
                        if (single_pin->GetLocked())
                        {
                            sys.constants.push_back(multiplier * (single_pin->current_rate - sum_locked));
                        }
                        else
                        {
                            sys.equations_coefficients.back()[vars.associated_variable_index.at(single_pin).first] = -1 * multiplier;
                            sys.constants.push_back(-1 * multiplier * sum_locked);
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
            sys.equations_coefficients.push_back(std::vector<FractionalNumber>(vars.num_variables));
            sys.equations_coefficients.back()[free_index] = 1;
            if (const auto link_it = vars.variable_of_link.find(free_index); link_it != vars.variable_of_link.end())
            {
                sys.constants.push_back(link_it->second->current_rate);
            }
            else if (pin == nullptr)
            {
                const auto default_it = vars.group_total_default.find(free_index);
                sys.constants.push_back(default_it != vars.group_total_default.end() ? default_it->second : FractionalNumber(0, 1));
            }
            else
            {
                sys.constants.push_back(pin->current_rate);
            }
        }
        processed_variables.insert(free_index);
        reduced.reduced_matrix = ReduceMatrix(sys.equations_coefficients, sys.constants, vars.num_variables);

        // We added more equations than variables, the only way we have a solution
        // is if all last lines are full 0 otherwise there is no solution
        if (vars.num_variables < sys.equations_coefficients.size())
        {
            bool solution_exists = true;
            for (size_t i = vars.num_variables; i < sys.equations_coefficients.size(); ++i)
            {
                for (size_t j = 0; j < vars.num_variables + 1; ++j)
                {
                    if (reduced.reduced_matrix[i][j] != 0)
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
                sys.equations_coefficients.resize(previous_num_equations);
                sys.constants.resize(previous_num_equations);
                reduced.reduced_matrix = ReduceMatrix(sys.equations_coefficients, sys.constants, vars.num_variables);
            }
        }
    }


    // Back substitution
    std::vector<FractionalNumber> solution(vars.num_variables);
    for (int i = vars.num_variables - 1; i >= 0; --i)
    {
        FractionalNumber sum = 0;
        for (int j = i + 1; j < vars.num_variables; ++j)
        {
            sum += reduced.reduced_matrix[i][j] * solution[j];
        }
        if (reduced.reduced_matrix[i][i].GetNumerator() == 0)
        {
            error_time = error_flow_duration;
            return false;
        }
        solution[i] = (reduced.reduced_matrix[i][vars.num_variables] - sum) / reduced.reduced_matrix[i][i];
    }

    // Check for negative solution
    for (const auto& n : in.nodes)
    {
        for (const auto& p : n->ins)
        {
            const auto it = vars.associated_variable_index.find(p.get());
            if (it != vars.associated_variable_index.end() && solution[it->second.first] < 0)
            {
                p->error = true;
                error_time = error_flow_duration;
            }
        }
        for (const auto& p : n->outs)
        {
            const auto it = vars.associated_variable_index.find(p.get());
            if (it != vars.associated_variable_index.end() && solution[it->second.first] < 0)
            {
                p->error = true;
                error_time = error_flow_duration;
            }
        }
    }
    // A branch of a fan-out can go negative while both of its pins stay positive - the pin only
    // sees the sum. Flag the edge itself, and mark both its ends so the user can see where.
    for (const auto& [link, index] : vars.link_variable_index)
    {
        if (solution[index] < 0)
        {
            if (link->start != nullptr) link->start->error = true;
            if (link->end != nullptr) link->end->error = true;
            error_time = error_flow_duration;
        }
    }
    if (error_time > 0.0f)
    {
        return false;
    }

    // For each link check if flow should be kept
    for (auto& l : in.links)
    {
        const auto it_start = vars.associated_variable_index.find(l->start);
        const auto it_end = vars.associated_variable_index.find(l->end);
        // Both ends kept their value, don't flow
        if (it_start != vars.associated_variable_index.end() && l->start->current_rate == solution[it_start->second.first] * it_start->second.second &&
            it_end != vars.associated_variable_index.end() && l->end->current_rate == solution[it_end->second.first] * it_end->second.second)
        {
            l->flow = std::nullopt;
        }
    }

    // Write the solved rate onto each edge that took part in this solve.
    for (auto& l : in.links)
    {
        const auto it = vars.link_variable_index.find(l.get());
        if (it != vars.link_variable_index.end())
        {
            l->current_rate = solution[it->second];
        }
    }

    // Set the updated new rates for each pin (and node)
    for (const auto& n : in.nodes)
    {
        for (const auto& p : n->ins)
        {
            const auto it = vars.associated_variable_index.find(p.get());
            if (it != vars.associated_variable_index.end())
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
            const auto it = vars.associated_variable_index.find(p.get());
            if (it != vars.associated_variable_index.end())
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

} // namespace rate_solver_detail
