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
    std::unordered_set<const Pin*> processed_pins;
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

        // If we haven't met this free pin yet
        if (processed_pins.find(pin) == processed_pins.end())
        {
            const auto group_it = (pin == nullptr) ? vars.group_membership.end() : vars.group_membership.find(pin);
            if (pin == nullptr)
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
            if (pin == nullptr)
            {
                const auto default_it = vars.group_total_default.find(free_index);
                sys.constants.push_back(default_it != vars.group_total_default.end() ? default_it->second : FractionalNumber(0, 1));
            }
            else
            {
                sys.constants.push_back(pin->current_rate);
            }
        }
        processed_pins.insert(pin);
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
