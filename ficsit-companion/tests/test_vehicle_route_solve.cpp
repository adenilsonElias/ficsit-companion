#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "domain/building.hpp"
#include "domain/fractional_number.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/rate_solver.hpp"
#include "domain/recipe.hpp" // Item, Recipe, CountedItem

#include "graph_test_helpers.hpp"

// ---------------------------------------------------------------------------
// Phase 2 — Vehicle Route Auto-Balance. Exercises RateSolver::Solve when the
// graph contains a vehicle route pool: editing one belt rate must settle the
// whole upstream -> loader -> pool -> unloader -> downstream chain in one solve.
//
// Vehicle plugs are structural (no Pin::link). Route links live in the `links`
// vector and are indexed on each station's `route_links`.
// ---------------------------------------------------------------------------

namespace
{
    using ax::NodeEditor::NodeId;
    using ax::NodeEditor::LinkId;

    // Build a Truck VehicleStationNode into `nodes` and return it (Load mode).
    VehicleStationNode* MakeStation(std::vector<std::unique_ptr<Node>>& nodes, IdGen& id_gen,
                                    VehicleStationNode::Mode mode)
    {
        auto gen = [&id_gen] { return id_gen(); };
        nodes.push_back(std::make_unique<VehicleStationNode>(
            NodeId(id_gen()), LogisticsNode::Kind::TruckStation, gen));
        auto* s = static_cast<VehicleStationNode*>(nodes.back().get());
        if (mode != VehicleStationNode::Mode::Load) s->SetMode(mode, gen);
        return s;
    }

    // Create a route (plug<->plug) link between two stations; index it on both.
    void RouteLink(std::vector<std::unique_ptr<Link>>& links, IdGen& id_gen,
                   VehicleStationNode* a, VehicleStationNode* b)
    {
        links.push_back(std::make_unique<Link>(LinkId(id_gen()), a->plug.get(), b->plug.get()));
        a->route_links.push_back(links.back().get());
        b->route_links.push_back(links.back().get());
    }
}

/// @test Driving a loader's cargo input settles the matching unloader output
///       across a route pool, in one solve.
/// @covers RateSolver pool BFS + per-group balance (supply -> T -> demand).
TEST_CASE("vehicle route: setting the loader drives the unloader", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    // D's output is in use (a downstream belt carries it) so it takes part in
    // the route balance; an idle, unconnected belt would be left at 0.
    D->outs[0]->current_rate = FractionalNumber(1, 1);
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, A->ins[0].get(),
                                      FractionalNumber(60, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(error_time == 0.0f);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1));
}

/// @test The route balance is symmetric: driving the unloader output settles
///       the loader input.
/// @covers RateSolver pool BFS reached from the demand side.
TEST_CASE("vehicle route: setting the unloader drives the loader", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    // The loader's input is in use (fed by an upstream belt) so it participates.
    A->ins[0]->current_rate = FractionalNumber(1, 1);
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, D->outs[0].get(),
                                      FractionalNumber(45, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(45, 1));
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(45, 1));
}

/// @test With two loaders supplying one unloader, fixing the route total (via
///       the unloader) keeps each loader's prior share of the total.
/// @covers RateSolver §6 supply-side ratio preservation relative to T.
TEST_CASE("vehicle route: two loaders keep their ratio when the total is set", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* B = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    B->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    // Prior rates establish a 1:2 ratio between the two loaders.
    A->ins[0]->current_rate = FractionalNumber(10, 1);
    B->ins[0]->current_rate = FractionalNumber(20, 1);
    RouteLink(links, id_gen, A, D);
    RouteLink(links, id_gen, B, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, D->outs[0].get(),
                                      FractionalNumber(60, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(20, 1)); // 1/3 of 60
    REQUIRE(B->ins[0]->current_rate == FractionalNumber(40, 1)); // 2/3 of 60
}

/// @test With one loader locked, driving the unloader total leaves the locked
///       loader fixed and the other absorbs the remainder.
/// @covers RateSolver §5 balance with a locked supply pin folded into the constant.
TEST_CASE("vehicle route: a locked loader lets the other absorb the remainder", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* B = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    B->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    A->ins[0]->current_rate = FractionalNumber(15, 1);
    A->ins[0]->SetLocked(true);
    // B is an in-use loader (fed upstream) so it participates and absorbs the rest.
    B->ins[0]->current_rate = FractionalNumber(1, 1);
    RouteLink(links, id_gen, A, D);
    RouteLink(links, id_gen, B, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, D->outs[0].get(),
                                      FractionalNumber(60, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(15, 1)); // locked, untouched
    REQUIRE(B->ins[0]->current_rate == FractionalNumber(45, 1)); // 60 - 15
}

/// @test With one loader feeding two unloaders, driving the loader splits the
///       total across the unloaders keeping their prior ratio.
/// @covers RateSolver §6 demand-side ratio preservation relative to T.
TEST_CASE("vehicle route: two unloaders split by ratio when the loader is set", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    VehicleStationNode* E = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    E->outs[0]->item = &rotor;
    D->outs[0]->current_rate = FractionalNumber(10, 1);
    E->outs[0]->current_rate = FractionalNumber(20, 1);
    RouteLink(links, id_gen, A, D);
    RouteLink(links, id_gen, A, E);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, A->ins[0].get(),
                                      FractionalNumber(90, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(90, 1));
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(30, 1)); // 1/3 of 90
    REQUIRE(E->outs[0]->current_rate == FractionalNumber(60, 1)); // 2/3 of 90
}

/// @test Two items share a pool but balance independently: editing the Rotor
///       side leaves the Screw side untouched.
/// @covers RateSolver per-(pool, item) grouping — one T per item.
TEST_CASE("vehicle route: items in a shared pool balance independently", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);
    Item screw("Screw", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    A->ins[1]->item = &screw;
    D->outs[0]->item = &rotor;
    D->outs[1]->item = &screw;
    // Screw side already balanced at 25; Rotor demand is in use (seeded) so it
    // takes part in the balance.
    A->ins[1]->current_rate = FractionalNumber(25, 1);
    D->outs[1]->current_rate = FractionalNumber(25, 1);
    D->outs[0]->current_rate = FractionalNumber(1, 1);
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, A->ins[0].get(),
                                      FractionalNumber(60, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1)); // Rotor balanced
    REQUIRE(A->ins[1]->current_rate == FractionalNumber(25, 1));  // Screw untouched
    REQUIRE(D->outs[1]->current_rate == FractionalNumber(25, 1)); // Screw untouched
}

/// @test Permissive balancing: when an item exists on only one side of the
///       pool it gets no equation, so it is never forced to zero.
/// @covers RateSolver permissive grouping (one-sided item omitted).
TEST_CASE("vehicle route: a one-sided item is never forced to zero", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);
    Item screw("Screw", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;  // supply only
    D->outs[0]->item = &screw; // demand only
    D->outs[0]->current_rate = FractionalNumber(25, 1);
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, A->ins[0].get(),
                                      FractionalNumber(60, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(60, 1)); // honoured
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(25, 1)); // not forced to 0
}

/// @test A full chain — machine -> loader cargo-in, unloader cargo-out -> sink —
///       settles in a single solve when the machine input is driven.
/// @covers RateSolver pool BFS reaching belt-link far ends on both sides.
TEST_CASE("vehicle route: a machine-to-sink chain settles in one solve", "[rate_solver][vehicle_route]")
{
    // 30 ore -> 20 plate recipe in a dummy building (no game data needed).
    Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
    Item ore("Iron Ore", "", 1);
    Item plate("Iron Plate", "", 2);
    std::vector<CountedItem> r_ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
    std::vector<CountedItem> r_outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
    Recipe recipe(r_ins, r_outs, &building, false, 4.0, "Recipe_IronPlate_C");

    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    nodes.push_back(std::make_unique<CraftNode>(NodeId(id_gen()), &recipe, id_gen));
    CraftNode* craft = static_cast<CraftNode*>(nodes.back().get());
    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    nodes.push_back(std::make_unique<SinkNode>(NodeId(id_gen()), id_gen, &plate));
    SinkNode* sink = static_cast<SinkNode*>(nodes.back().get());

    A->ins[0]->item = &plate;
    D->outs[0]->item = &plate;

    links.push_back(MakeLink(id_gen(), craft->outs[0].get(), A->ins[0].get())); // machine -> loader
    links.push_back(MakeLink(id_gen(), D->outs[0].get(), sink->ins[0].get()));  // unloader -> sink
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, craft->ins[0].get(),
                                      FractionalNumber(30, 1), error_time, 1.0f);

    REQUIRE(ok);
    REQUIRE(craft->ins[0]->current_rate == FractionalNumber(30, 1));
    REQUIRE(craft->outs[0]->current_rate == FractionalNumber(20, 1)); // one machine
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(20, 1));      // across belt
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(20, 1));     // across pool
    REQUIRE(sink->ins[0]->current_rate == FractionalNumber(20, 1));   // across belt
}

/// @test An over-constrained route (locked supply total != locked demand total)
///       has no solution, so the solve is rejected and error_time is set.
/// @covers RateSolver rejection path with route balance equations.
TEST_CASE("vehicle route: a locked supply/demand mismatch is rejected", "[rate_solver][vehicle_route]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = MakeStation(nodes, id_gen, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    A->ins[0]->current_rate = FractionalNumber(100, 1);
    A->ins[0]->SetLocked(true);
    D->outs[0]->current_rate = FractionalNumber(50, 1);
    D->outs[0]->SetLocked(true);
    RouteLink(links, id_gen, A, D);

    float error_time = 0.0f;
    const bool ok = RateSolver::Solve(nodes, links, A->ins[0].get(),
                                      FractionalNumber(100, 1), error_time, 3.0f);

    REQUIRE(ok == false);
    REQUIRE(error_time == 3.0f);
}
