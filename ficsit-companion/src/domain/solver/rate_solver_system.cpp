#include "domain/solver/rate_solver_internal.hpp"
#include "domain/solver/linear_solve.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/graph/link.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

namespace rate_solver_detail
{
    LinearSystem BuildLinearSystem(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars)
    {
        LinearSystem sys;
        // Now we can create the main equations linked to this graph operation
        // The end goal is to solve sys.equations_coefficients * x = sys.constants
        // Used to prevent double processing the same link (it would work with duplicated equations but more efficient to avoid creating them in the first place)
        std::unordered_set<const Link*> processed_links;
        // used to prevent double processing the same Merger/CustomSplitter node
        std::unordered_set<const Node*> processed_multi_node;

        // First equation is from the user input
        // rate * user variable = user constraint
        {
            std::vector<FractionalNumber> equation(vars.num_variables);
            const auto& variable = vars.associated_variable_index.at(in.constraint_pin);
            equation[variable.first] = variable.second;
            sys.equations_coefficients.push_back(equation);
            sys.constants.push_back(in.constraint_value);
        }

        // Then process all pins to add an equality equation per link and a balance equation for all merger/customsplitter
        for (const Pin* pin : seed.relevant_pins)
        {
            if (pin->link != nullptr && processed_links.find(pin->link) == processed_links.end())
            {
                // Add an equation for equality
                // X - Y = 0
                std::vector<FractionalNumber> equation(vars.num_variables);
                const std::pair<size_t, FractionalNumber>& start_variable = vars.associated_variable_index.at(pin->link->start);
                const std::pair<size_t, FractionalNumber>& end_variable = vars.associated_variable_index.at(pin->link->end);
                equation[start_variable.first] = start_variable.second;
                equation[end_variable.first] = -1 * end_variable.second;
                sys.equations_coefficients.push_back(equation);
                sys.constants.push_back(0);
                processed_links.insert(pin->link);
            }

            if ((pin->node->GetKind() == Node::Kind::Merger || pin->node->GetKind() == Node::Kind::CustomSplitter) && processed_multi_node.find(pin->node) == processed_multi_node.end())
            {
                // Add an equation sum(inputs) - sum(outputs) = 0
                std::vector<FractionalNumber> equation(vars.num_variables);
                FractionalNumber constant = 0;

                for (const std::unique_ptr<Pin>& i : pin->node->ins)
                {
                    if (i->GetLocked())
                    {
                        constant -= i->current_rate;
                    }
                    else
                    {
                        equation[vars.associated_variable_index.at(i.get()).first] = 1;
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
                        equation[vars.associated_variable_index.at(o.get()).first] = -1;
                    }
                }
                sys.equations_coefficients.push_back(equation);
                sys.constants.push_back(constant);
                processed_multi_node.insert(pin->node);
            }
        }

        // Per route-group balance: each group behaves as merger(supply -> T) feeding
        // splitter(T -> demand). Locked pins fold into the constant exactly as the
        // merger/splitter balance does above.
        //   sum(unlocked supply) - T = -sum(locked supply)
        //   T - sum(unlocked demand) =  sum(locked demand)
        for (size_t gi = 0; gi < seed.active_groups.size(); ++gi)
        {
            const ActiveGroup& g = seed.active_groups[gi];
            const size_t T = vars.group_total_index[gi];
            {
                std::vector<FractionalNumber> equation(vars.num_variables);
                FractionalNumber constant = 0;
                for (Pin* p : g.supply)
                {
                    if (p->GetLocked()) constant -= p->current_rate;
                    else equation[vars.associated_variable_index.at(p).first] += 1;
                }
                equation[T] = -1;
                sys.equations_coefficients.push_back(equation);
                sys.constants.push_back(constant);
            }
            {
                std::vector<FractionalNumber> equation(vars.num_variables);
                FractionalNumber constant = 0;
                equation[T] = 1;
                for (Pin* p : g.demand)
                {
                    if (p->GetLocked()) constant += p->current_rate;
                    else equation[vars.associated_variable_index.at(p).first] += -1;
                }
                sys.equations_coefficients.push_back(equation);
                sys.constants.push_back(constant);
            }
        }

        return sys;
    }

    std::optional<ReducedSystem> ReduceAndCheck(const LinearSystem& sys, const VariableMapping& vars,
                                                float& error_time, float error_flow_duration)
    {
        ReducedSystem reduced;
        // Reduce the equation matrix using gaussian elimination (pure numeric
        // kernel extracted to domain/linear_solve).
        reduced.reduced_matrix = ReduceMatrix(sys.equations_coefficients, sys.constants, vars.num_variables);

        // We had more equations than variables, the only way we have a solution
        // is if all last lines are full 0 otherwise there is no solution
        if (vars.num_variables < sys.equations_coefficients.size())
        {
            for (size_t i = vars.num_variables; i < sys.equations_coefficients.size(); ++i)
            {
                for (size_t j = 0; j < vars.num_variables + 1; ++j)
                {
                    if (reduced.reduced_matrix[i][j] != 0)
                    {
                        error_time = error_flow_duration;
                        return std::nullopt;
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
        for (size_t i = 0; i < reduced.reduced_matrix.size(); ++i)
        {
            bool all_coeffs_zero = true;
            for (size_t j = 0; j < vars.num_variables; ++j)
            {
                if (reduced.reduced_matrix[i][j] != 0) { all_coeffs_zero = false; break; }
            }
            if (all_coeffs_zero && reduced.reduced_matrix[i][vars.num_variables] != 0)
            {
                error_time = error_flow_duration;
                return std::nullopt;
            }
        }

        return reduced;
    }
}
