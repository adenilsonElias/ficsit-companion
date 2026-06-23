#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

#include "domain/core/fractional_number.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/solver/rate_solver_internal.hpp"

#include "graph_test_helpers.hpp"

using namespace rate_solver_detail;

TEST_CASE("SeedRelevantPins pulls a merger's pins into the relevant set", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);

    REQUIRE(seed.relevant_pins.count(merger->outs[0].get()) == 1);
    // All input pins also pulled in (via multi_pin_maybe_constrained).
    REQUIRE(seed.relevant_pins.count(merger->ins[0].get()) == 1);
    REQUIRE(seed.relevant_pins.count(merger->ins[1].get()) == 1);
    REQUIRE(seed.relevant_pins.count(merger->ins[2].get()) == 1);
}

TEST_CASE("AssignVariables gives a merger one variable per pin at ratio 1", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);

    // 3 inputs + 1 output = 4 pin variables, each ratio 1.
    REQUIRE(vars.num_variables == 4);
    REQUIRE(vars.associated_variable_index.at(merger->outs[0].get()).second == FractionalNumber(1, 1));
}

TEST_CASE("BuildLinearSystem then ReduceAndCheck solves a lone merger", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);

    // User-constraint equation + merger balance => at least 2 equations.
    REQUIRE(sys.equations_coefficients.size() >= 2);

    float error_time = 0.0f;
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, 1.0f);
    REQUIRE(reduced.has_value());
    REQUIRE(error_time == 0.0f);
}

TEST_CASE("ApplyResults writes back the even split for a lone merger", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);
    float error_time = 0.0f;
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, 1.0f);
    REQUIRE(reduced.has_value());

    const bool ok = ApplyResults(in, seed, vars, sys, *reduced, error_time, 1.0f);
    REQUIRE(ok);
    REQUIRE(merger->outs[0]->current_rate == FractionalNumber(12, 1));
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(4, 1));
}

// ---------------------------------------------------------------------------
// Failure-path: ReduceAndCheck returns nullopt for an over-constrained system.
//
// Topology: lone MergerNode, all three inputs locked at 5/min each.
// User constraint: drive outs[0] to 10/min.
//
// The balance equation is:  -(outs[0]) = -(5+5+5) = -15
//   => outs[0] = 15
// The user-constraint equation is:
//   outs[0] = 10
//
// These two equations are contradictory (one variable, two different
// values).  After Gaussian elimination the "extra" row becomes
//   [0 | -5]  (all coefficients zero, non-zero constant),
// which ReduceAndCheck detects as an inconsistent system and returns
// std::nullopt while setting error_time to error_flow_duration.
//
// This mirrors the "solver rejects update that requires negative input
// rates" case in test_rate_solver.cpp but exercises the phase function
// directly rather than through the full Solve() orchestrator.
// ---------------------------------------------------------------------------
TEST_CASE("ReduceAndCheck returns nullopt for incompatible locked-input totals", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // Lock all three inputs at 5/min (sum = 15); drive the output to 10/min.
    // The merger balance and the user constraint then contradict each other.
    merger->ins[0]->current_rate = FractionalNumber(5, 1);
    merger->ins[0]->SetLocked(true);
    merger->ins[1]->current_rate = FractionalNumber(5, 1);
    merger->ins[1]->SetLocked(true);
    merger->ins[2]->current_rate = FractionalNumber(5, 1);
    merger->ins[2]->SetLocked(true);

    FractionalNumber drive_rate(10, 1);
    SolveInputs in{ nodes, links, merger->outs[0].get(), drive_rate };
    const SeedResult seed  = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);

    float error_time = 0.0f;
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, 7.5f);

    REQUIRE_FALSE(reduced.has_value());
    REQUIRE(error_time == 7.5f);
}
