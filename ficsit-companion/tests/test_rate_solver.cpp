#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "domain/gamedata/building.hpp"
#include "domain/core/fractional_number.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/solver/rate_solver.hpp"
#include "domain/gamedata/recipe.hpp"

#include "graph_test_helpers.hpp"

// ---------------------------------------------------------------------------
// Locks in the exact rational behavior of the propagation algorithm extracted
// from ProductionApp::UpdateNodesRate into RateSolver::Solve.
//
// Topologies use nullptr items (the solver never touches Pin::item) and pins
// default to current_rate 0 / base_rate 0 / unlocked. Merger/CustomSplitter
// variables use ratio 1, so item/base_rate are irrelevant to these cases.
// ---------------------------------------------------------------------------

namespace
{
    // Sum the current_rate of every pin in a list.
    FractionalNumber SumRates(const std::vector<std::unique_ptr<Pin>>& pins)
    {
        FractionalNumber total(0, 1);
        for (const auto& p : pins)
        {
            total += p->current_rate;
        }
        return total;
    }
}

/// @test   Driving a merger's single output to 12/min with no prior input rates splits it evenly
///         (4+4+4) across its three inputs, succeeds, leaves error_time at 0, and conserves flow
///         (Σ inputs == output).
/// @covers RateSolver::Solve on a lone MergerNode — even distribution across unlocked inputs and
///         the merger balance constraint, with exact rational arithmetic.
TEST_CASE("merger distributes output rate evenly across inputs", "[rate_solver]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    // Merger: 3 inputs + 1 output, all starting at rate 0.
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // Drive the single output pin to 12/min.
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, merger->outs[0].get(),
                                      FractionalNumber(12, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(merger->outs[0]->current_rate == FractionalNumber(12, 1));
    // No prior rates => even split across the 3 inputs.
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(4, 1));
    REQUIRE(merger->ins[1]->current_rate == FractionalNumber(4, 1));
    REQUIRE(merger->ins[2]->current_rate == FractionalNumber(4, 1));
    // Conservation: sum of inputs equals the output.
    REQUIRE(SumRates(merger->ins) == merger->outs[0]->current_rate);
}

/// @test   Driving a custom splitter's single input to 9/min with no prior output rates splits it
///         evenly (3+3+3) across its three outputs, succeeds, and conserves flow (Σ outputs == input).
/// @covers RateSolver::Solve on a lone CustomSplitterNode — the mirror image of the merger case
///         (even distribution across outputs and the splitter balance constraint).
TEST_CASE("custom splitter distributes input rate evenly across outputs", "[rate_solver]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    // CustomSplitter: 1 input + 3 outputs, all starting at rate 0.
    nodes.push_back(std::make_unique<CustomSplitterNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    CustomSplitterNode* splitter = static_cast<CustomSplitterNode*>(nodes.back().get());

    // Drive the single input pin to 9/min.
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, splitter->ins[0].get(),
                                      FractionalNumber(9, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(splitter->ins[0]->current_rate == FractionalNumber(9, 1));
    // No prior rates => even split across the 3 outputs.
    REQUIRE(splitter->outs[0]->current_rate == FractionalNumber(3, 1));
    REQUIRE(splitter->outs[1]->current_rate == FractionalNumber(3, 1));
    REQUIRE(splitter->outs[2]->current_rate == FractionalNumber(3, 1));
    // Conservation: sum of outputs equals the input.
    REQUIRE(SumRates(splitter->outs) == splitter->ins[0]->current_rate);
}

/// @test   With a splitter output linked into a merger input, driving the splitter's input to 12/min
///         splits to 4 per output and propagates that 4 across the link so both linked pins hold the
///         same rate.
/// @covers RateSolver::Solve across a Link between two organizer nodes — the cross-node propagation
///         step plus the invariant that the two pins joined by a link end up equal.
TEST_CASE("rate flows across a splitter->merger link with equal linked-pin rates", "[rate_solver]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CustomSplitterNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    CustomSplitterNode* splitter = static_cast<CustomSplitterNode*>(nodes.back().get());

    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // splitter output[0] -> merger input[0]
    links.push_back(MakeLink(id_gen(), splitter->outs[0].get(), merger->ins[0].get()));

    // Drive the splitter input to 12/min.
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, splitter->ins[0].get(),
                                      FractionalNumber(12, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(splitter->ins[0]->current_rate == FractionalNumber(12, 1));
    // Even split: each splitter output gets 4.
    REQUIRE(splitter->outs[0]->current_rate == FractionalNumber(4, 1));
    // The two linked pins must end up with the same rate.
    REQUIRE(splitter->outs[0]->current_rate == merger->ins[0]->current_rate);
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(4, 1));
}

/// @test   With one merger input locked at 3/min, driving the output to 12/min leaves the locked
///         input untouched and splits the remaining 9 evenly (9/2 each) across the two unlocked
///         inputs, while conservation still holds.
/// @covers RateSolver::Solve honoring Pin lock flags — locked pins are treated as fixed constants
///         and only unlocked pins absorb the remainder. Exercises non-integer (9/2) exact results.
TEST_CASE("merger keeps a locked input fixed and splits the remainder", "[rate_solver]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // Lock the third input at 3/min; the solver must leave it untouched.
    merger->ins[2]->current_rate = FractionalNumber(3, 1);
    merger->ins[2]->SetLocked(true);

    // Drive the output to 12/min.
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, merger->outs[0].get(),
                                      FractionalNumber(12, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(merger->outs[0]->current_rate == FractionalNumber(12, 1));
    // Locked input is unchanged.
    REQUIRE(merger->ins[2]->current_rate == FractionalNumber(3, 1));
    // Remaining 9 split evenly between the two unlocked inputs => 9/2 each.
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(9, 2));
    REQUIRE(merger->ins[1]->current_rate == FractionalNumber(9, 2));
    // Conservation still holds.
    REQUIRE(SumRates(merger->ins) == merger->outs[0]->current_rate);
}

/// @test   An infeasible request that would force negative rates is rejected: with one input locked
///         at 100/min, driving the output down to 10/min (which implies -45 on each remaining input)
///         makes Solve return false and write the caller's error_flow_duration (7.5) into error_time.
/// @covers RateSolver::Solve's negative-solution guard and its error-signalling contract (return
///         false + set error_time), preventing physically impossible negative flows from being applied.
TEST_CASE("solver rejects update that requires negative input rates", "[rate_solver]")
{
    // Topology: single MergerNode (3 inputs / 1 output).
    // ins[0] is locked at 100/min. We then drive outs[0] to 10/min.
    //
    // Merger balance:  ins[0] + ins[1] + ins[2] = outs[0]
    //                  100    + ins[1] + ins[2] = 10
    //                  => ins[1] + ins[2]        = -90
    //
    // The solver distributes the remainder evenly: ins[1] = ins[2] = -45.
    // Negative rates are physically impossible; the solver must detect this
    // via the negative-solution check (rate_solver.cpp, "Check for negative
    // solution" block) and return false while setting error_time to the
    // caller-supplied error_flow_duration value.

    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // Lock ins[0] at 100/min — intentionally larger than the target output.
    merger->ins[0]->current_rate = FractionalNumber(100, 1);
    merger->ins[0]->SetLocked(true);

    // Use a distinctive error_flow_duration so we can verify the out-param
    // was written by the exact substitution the refactor changed.
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, merger->outs[0].get(),
                                      FractionalNumber(10, 1), error_time, 7.5f);

    // The update is infeasible; Solve must signal rejection.
    REQUIRE(ok == false);
    // error_time must be set to the passed error_flow_duration (not left at 0).
    REQUIRE(error_time == 7.5f);
}

// ---------------------------------------------------------------------------
// CraftNode topologies exercise the per-node "ratio variable" path (one
// variable shared across all pins of a craft node, scaled by base_rate).
// ---------------------------------------------------------------------------

namespace
{
    // A 30 ore -> 20 plate recipe in a dummy building (no game data needed).
    struct CraftFixture
    {
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        std::vector<CountedItem> ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe recipe{ ins, outs, &building, false, 4.0, "Recipe_IronPlate_C" };
    };
}

/// @test   For a 30-ore→20-plate craft, driving the plate output to 40/min scales the ore input to
///         60/min (40 × 30/20), succeeding with error_time 0.
/// @covers RateSolver::Solve on a CraftNode in the output-driven direction — the per-node ratio
///         variable that ties every pin to base_rate via the recipe quantities.
TEST_CASE("craft node scales its input when the output is driven", "[rate_solver]")
{
    CraftFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.recipe, id_gen));
    CraftNode* craft = static_cast<CraftNode*>(nodes.back().get());

    // Drive the plate output to 40/min (2 machines' worth).
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, craft->outs[0].get(),
                                      FractionalNumber(40, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(craft->outs[0]->current_rate == FractionalNumber(40, 1));
    // Input scales by the recipe ratio 30/20: 40 * 30/20 = 60.
    REQUIRE(craft->ins[0]->current_rate == FractionalNumber(60, 1));
}

/// @test   For the same 30-ore→20-plate craft, driving the ore input to 15/min scales the plate
///         output to 10/min (15 × 20/30).
/// @covers RateSolver::Solve on a CraftNode in the input-driven direction — the reverse of the
///         previous case, confirming the recipe ratio applies symmetrically.
TEST_CASE("craft node scales its output when the input is driven", "[rate_solver]")
{
    CraftFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.recipe, id_gen));
    CraftNode* craft = static_cast<CraftNode*>(nodes.back().get());

    // Drive the ore input to 15/min (half a machine).
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, craft->ins[0].get(),
                                      FractionalNumber(15, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(craft->ins[0]->current_rate == FractionalNumber(15, 1));
    // Output scales by 20/30: 15 * 20/30 = 10.
    REQUIRE(craft->outs[0]->current_rate == FractionalNumber(10, 1));
}

/// @test   With a craft's plate output linked into a merger, driving the craft input to 30/min
///         (exactly one machine) yields 20 plate out and carries that 20 across the link into the
///         merger input.
/// @covers RateSolver::Solve combining a CraftNode ratio with cross-link propagation into an
///         organizer node — the recipe scaling and link transfer working together end-to-end.
TEST_CASE("craft output feeding a merger propagates the recipe ratio across the link", "[rate_solver]")
{
    CraftFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.recipe, id_gen));
    CraftNode* craft = static_cast<CraftNode*>(nodes.back().get());

    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    // craft plate output -> merger input[0]
    links.push_back(MakeLink(id_gen(), craft->outs[0].get(), merger->ins[0].get()));

    // Drive the craft input to 30/min (exactly one machine).
    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, craft->ins[0].get(),
                                      FractionalNumber(30, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(craft->ins[0]->current_rate == FractionalNumber(30, 1));
    // One machine -> 20 plate out, carried across the link into the merger.
    REQUIRE(craft->outs[0]->current_rate == FractionalNumber(20, 1));
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(20, 1));
}

// ---------------------------------------------------------------------------
// Multi-link pins (Production Planner fan-out / fan-in). A pin's rate is the sum
// of its links' rates, so one output can feed several consumers with no splitter
// node, and one input can be fed by several producers with no merger node.
// ---------------------------------------------------------------------------

namespace
{
    // A 30 ore -> 20 plate producer, and a 20 plate -> 10 rod consumer. Two consumer nodes will
    // pull from the SAME producer output pin: that is the fan-out.
    struct FanOutFixture
    {
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        Item rod{ "Iron Rod", "", 3 };

        std::vector<CountedItem> prod_ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> prod_outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe producer{ prod_ins, prod_outs, &building, false, 4.0, "Recipe_IronPlate_C" };

        std::vector<CountedItem> cons_ins{ CountedItem(&plate, FractionalNumber(20, 1)) };
        std::vector<CountedItem> cons_outs{ CountedItem(&rod, FractionalNumber(10, 1)) };
        Recipe consumer{ cons_ins, cons_outs, &building, false, 4.0, "Recipe_IronRod_C" };
    };
}

/// @test   Fan-out: with two consumers on one output pin, pinning one consumer's demand leaves
///         the other alone — the producer's output becomes the sum of both demands, and each
///         link carries its own share.
/// @covers RateSolver::Solve on a multi-link pin: the per-pin balance equation (the sum of a
///         pin's links equals the pin's rate) together with the existing free-variable rule
///         "keep the current value". This is the case that motivated the feature: one Iron Rod
///         machine feeding both Screw and Rotor without a splitter in between.
TEST_CASE("fan-out sums demand into the producer", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_b = static_cast<CraftNode*>(nodes.back().get());

    // Both consumers hang off the SAME producer output pin.
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_a->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_b->ins[0].get()));
    Link* link_b = links.back().get();

    REQUIRE(producer->outs[0]->links.size() == 2);

    float error_time = 0.0f;

    // Consumer A asks for 40 plate/min. B asks for nothing yet: its variable is free, so the
    // solver holds it at 0.
    REQUIRE(RateSolver::Solve(nodes, links, consumer_a->ins[0].get(),
                              FractionalNumber(40, 1), error_time, 1.0f));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(40, 1));

    // Now consumer B asks for 20 plate/min. A's variable is free and the solver holds it at 40.
    // That is where "production ramps up" comes from, instead of "A gets squeezed".
    REQUIRE(RateSolver::Solve(nodes, links, consumer_b->ins[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(error_time == 0.0f);

    REQUIRE(consumer_a->ins[0]->current_rate == FractionalNumber(40, 1)); // not squeezed
    REQUIRE(consumer_b->ins[0]->current_rate == FractionalNumber(20, 1));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1));  // 40 + 20
    // The producer scaled with it: 60 plate needs 90 ore (the 30/20 recipe ratio).
    REQUIRE(producer->ins[0]->current_rate == FractionalNumber(90, 1));

    // Each edge carries its own share...
    REQUIRE(link_a->current_rate == FractionalNumber(40, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(20, 1));
    // ...and the central invariant holds: a pin's rate is the sum of its links' rates.
    REQUIRE(link_a->current_rate + link_b->current_rate == producer->outs[0]->current_rate);
}

/// @test   Fan-in: two producers on one input pin sum into the consumer.
/// @covers The mirror of the fan-out case — the same per-pin balance equation, this time on an
///         input pin (an implicit merger). Proves the balance is not a special case of outputs.
TEST_CASE("fan-in sums supply into the consumer", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer_b = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer = static_cast<CraftNode*>(nodes.back().get());

    // Both producers hang off the SAME consumer input pin.
    links.push_back(MakeLink(id_gen(), producer_a->outs[0].get(), consumer->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer_b->outs[0].get(), consumer->ins[0].get()));
    Link* link_b = links.back().get();

    REQUIRE(consumer->ins[0]->links.size() == 2);

    float error_time = 0.0f;
    REQUIRE(RateSolver::Solve(nodes, links, producer_a->outs[0].get(),
                              FractionalNumber(30, 1), error_time, 1.0f));
    REQUIRE(RateSolver::Solve(nodes, links, producer_b->outs[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(error_time == 0.0f);

    REQUIRE(producer_a->outs[0]->current_rate == FractionalNumber(30, 1));
    REQUIRE(producer_b->outs[0]->current_rate == FractionalNumber(20, 1));
    REQUIRE(consumer->ins[0]->current_rate == FractionalNumber(50, 1)); // 30 + 20
    REQUIRE(link_a->current_rate == FractionalNumber(30, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(20, 1));
}

/// @test   Raising a fan-out pin's own rate re-splits the new total in the branches' prior
///         ratio: a 40/20 split driven to 90 becomes 60/30, not 40/50.
/// @covers The free-link-variable rule. Driving the shared pin leaves the branch split
///         underdetermined; without a ratio rule the solver would hold one branch at its old
///         value and dump the entire difference on the other, which is not a split at all.
TEST_CASE("raising a fan-out pin keeps the branch ratio", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_b = static_cast<CraftNode*>(nodes.back().get());

    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_a->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_b->ins[0].get()));
    Link* link_b = links.back().get();

    float error_time = 0.0f;
    // Establish the 40 / 20 split (total 60).
    REQUIRE(RateSolver::Solve(nodes, links, consumer_a->ins[0].get(),
                              FractionalNumber(40, 1), error_time, 1.0f));
    REQUIRE(RateSolver::Solve(nodes, links, consumer_b->ins[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1));

    // Now drive the shared output pin itself to 90/min.
    REQUIRE(RateSolver::Solve(nodes, links, producer->outs[0].get(),
                              FractionalNumber(90, 1), error_time, 1.0f));
    REQUIRE(error_time == 0.0f);

    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(90, 1));
    // The 2:1 ratio is preserved: 90 splits as 60 / 30.
    REQUIRE(link_a->current_rate == FractionalNumber(60, 1));
    REQUIRE(link_b->current_rate == FractionalNumber(30, 1));
    REQUIRE(consumer_a->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(consumer_b->ins[0]->current_rate == FractionalNumber(30, 1));
}

/// @test   A locked branch of a fan-out keeps its rate: it is excluded from the re-split, and the
///         other branches share whatever the shared pin's new total leaves over.
/// @covers How the lock interacts with a fan-out re-split. A locked branch folds into the
///         equation's constant, exactly as a locked pin does on a CustomSplitter. This is also
///         the guarantee that locking one consumer pins it while you tune the rest.
TEST_CASE("a locked branch keeps its rate under fan-out", "[rate_solver][multilink]")
{
    FanOutFixture fx;
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_a = static_cast<CraftNode*>(nodes.back().get());
    nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* consumer_b = static_cast<CraftNode*>(nodes.back().get());

    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_a->ins[0].get()));
    Link* link_a = links.back().get();
    links.push_back(MakeLink(id_gen(), producer->outs[0].get(), consumer_b->ins[0].get()));
    Link* link_b = links.back().get();

    float error_time = 0.0f;
    REQUIRE(RateSolver::Solve(nodes, links, consumer_a->ins[0].get(),
                              FractionalNumber(40, 1), error_time, 1.0f));
    REQUIRE(RateSolver::Solve(nodes, links, consumer_b->ins[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1));

    // Pin branch A at 40. The lock stops at the fanning pin, so the producer and branch B stay
    // free - that is what makes this scenario expressible at all.
    consumer_a->ins[0]->SetLocked(true);
    REQUIRE_FALSE(producer->outs[0]->GetLocked());
    REQUIRE_FALSE(consumer_b->ins[0]->GetLocked());

    REQUIRE(RateSolver::Solve(nodes, links, producer->outs[0].get(),
                              FractionalNumber(90, 1), error_time, 1.0f));
    REQUIRE(error_time == 0.0f);

    REQUIRE(link_a->current_rate == FractionalNumber(40, 1)); // locked, untouched
    REQUIRE(link_b->current_rate == FractionalNumber(50, 1)); // takes the whole remainder
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(90, 1));
}
