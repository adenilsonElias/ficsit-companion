#pragma once

#include "domain/core/fractional_number.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Node;
struct Link;
struct Pin;

namespace rate_solver_detail
{
    struct ActiveGroup
    {
        std::vector<Pin*> supply;
        std::vector<Pin*> demand;
    };

    struct SolveInputs
    {
        std::vector<std::unique_ptr<Node>>& nodes;
        std::vector<std::unique_ptr<Link>>& links;
        const Pin* constraint_pin;
        const FractionalNumber& constraint_value;
    };

    struct SeedResult
    {
        std::unordered_set<const Pin*> relevant_pins;
        // Every link incident to a relevant pin. Each one gets its own variable, so a pin with
        // fan-out can split its rate across branches instead of forcing them all equal.
        std::unordered_set<const Link*> relevant_links;
        std::unordered_set<const Pin*> multi_pin_constrained;
        std::vector<ActiveGroup> active_groups;
    };

    struct VariableMapping
    {
        std::unordered_map<const Pin*, std::pair<std::size_t, FractionalNumber>> associated_variable_index;
        // One variable per relevant link: the rate on that edge. On a single-link pin the pin
        // balance equation ties it to the pin's own rate and the system collapses to what it was
        // before; on a pin with fan-out it is the degree of freedom that splits the branches.
        std::unordered_map<const Link*, std::size_t> link_variable_index;
        // The inverse. A link variable has no pin, so reversed_variable_map is null at its index
        // (same as a route total T); this is how ApplyResults tells the two apart.
        std::unordered_map<std::size_t, const Link*> variable_of_link;
        std::size_t num_variables = 0;
        std::vector<std::size_t> group_total_index;
        std::unordered_map<std::size_t, FractionalNumber> group_total_default;
        std::unordered_map<const Pin*, std::pair<std::size_t, bool>> group_membership;
        std::vector<const Pin*> reversed_variable_map;
    };

    struct LinearSystem
    {
        std::vector<std::vector<FractionalNumber>> equations_coefficients;
        std::vector<FractionalNumber> constants;
    };

    struct ReducedSystem
    {
        std::vector<std::vector<FractionalNumber>> reduced_matrix;
    };

    SeedResult SeedRelevantPins(const SolveInputs& in);
    VariableMapping AssignVariables(const SeedResult& seed);
    LinearSystem BuildLinearSystem(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars);
    std::optional<ReducedSystem> ReduceAndCheck(const LinearSystem& sys, const VariableMapping& vars,
                                                float& error_time, float error_flow_duration);
    bool ApplyResults(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars,
                      LinearSystem& sys, ReducedSystem& reduced,
                      float& error_time, float error_flow_duration);
}
