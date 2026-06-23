#include "domain/solver/rate_solver.hpp"
#include "domain/solver/rate_solver_internal.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/graph/link.hpp"

#include <cstdio>   // fprintf, used by the DEBUG_PROPAGATION trace block in Solve
#include <memory>
#include <optional>
#include <vector>

#define DEBUG_PROPAGATION 0

namespace
{
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
    using namespace rate_solver_detail;
    ResetGraphState(nodes, links, error_time);
    SolveInputs in{ nodes, links, constraint_pin, constraint_value };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, error_flow_duration);
    if (!reduced) return false;
    return ApplyResults(in, seed, vars, sys, *reduced, error_time, error_flow_duration);
}
