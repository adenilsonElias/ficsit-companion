#include <catch2/catch_test_macros.hpp>

#include "domain/graph_model.hpp"
#include "graph_test_helpers.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp" // Item
#include "domain/vehicle_route.hpp"

namespace
{
    // Build a Truck VehicleStationNode into the model and return a raw pointer.
    VehicleStationNode* AddStation(GraphModel& g, VehicleStationNode::Mode mode)
    {
        auto gen = [&g] { return g.GetNextId(); };
        auto s = std::make_unique<VehicleStationNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::TruckStation, gen);
        if (mode != VehicleStationNode::Mode::Load) s->SetMode(mode, gen);
        VehicleStationNode* raw = s.get();
        g.nodes.push_back(std::move(s));
        return raw;
    }
}

/// @test   GraphModel hands out monotonically increasing ids — two successive calls differ by one.
/// @covers GraphModel::GetNextId. Guards the unique-id source that node/link/pin construction
///         depends on to avoid editor id collisions.
TEST_CASE("GraphModel::GetNextId increments", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);

    const auto first = g.GetNextId();
    const auto second = g.GetNextId();

    REQUIRE(second == first + 1);
}

/// @test   Deleting a node also tears down every link touching it: the node leaves the model, the
///         incident link is removed, the surviving pin's back-reference is cleared, and both the
///         node and link deletions are forwarded to the editor backend.
/// @covers GraphModel::DeleteNode and its cascade — model bookkeeping (nodes/links vectors),
///         Pin::link cleanup on the far endpoint, and the IEditorBackend::DeleteNode/DeleteLink
///         calls (verified via FakeEditorBackend). Protects against dangling links/pins after node
///         removal.
TEST_CASE("GraphModel::DeleteNode removes node and its incident links", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    IdGen ids;

    auto source = std::make_unique<CustomSplitterNode>(g.GetNextId(), std::ref(ids));
    auto target = std::make_unique<MergerNode>(g.GetNextId(), std::ref(ids));

    Pin* source_out = source->outs[0].get();
    Pin* target_in = target->ins[0].get();
    const ax::NodeEditor::NodeId source_id = source->id;
    const ax::NodeEditor::LinkId link_id(99);

    g.nodes.push_back(std::move(source));
    g.nodes.push_back(std::move(target));
    g.links.push_back(MakeLink(link_id.Get(), source_out, target_in));

    g.DeleteNode(source_id);

    REQUIRE(g.nodes.size() == 1);
    REQUIRE(g.links.empty());
    REQUIRE(target_in->link == nullptr);
    REQUIRE(fake.deleted_nodes.size() == 1);
    REQUIRE(fake.deleted_nodes[0] == source_id);
    REQUIRE(fake.deleted_links.size() == 1);
    REQUIRE(fake.deleted_links[0] == link_id);
}

/// @test Creating a route (plug<->plug) link triggers a solve that balances the
///       new pool: a loader already supplying 60 drives the freshly connected
///       unloader to 60.
/// @covers GraphModel::CreateLink route-link branch — re-solve on connection.
TEST_CASE("GraphModel::CreateLink balances a new route", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    A->ins[0]->current_rate = FractionalNumber(60, 1);
    D->outs[0]->item = &rotor;
    // D's first output is in use (a downstream belt carries it) so it takes part
    // in the balance on connect.
    D->outs[0]->current_rate = FractionalNumber(1, 1);

    float error_time = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), true, error_time, 1.0f);

    REQUIRE(g.links.size() == 1);
    REQUIRE(A->route_links.size() == 1);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1)); // balanced on connect
}

/// @test In restore mode (trigger_update == false, used by session load and
///       group paste) creating a route link does NOT propagate items or
///       re-solve — the saved/pasted state is authoritative and must be left
///       untouched apart from the route bookkeeping.
/// @covers GraphModel::CreateLink route-link branch gated on trigger_update.
TEST_CASE("GraphModel::CreateLink leaves restored state untouched in restore mode", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item plate("Iron Plate", "", 2);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &plate;
    A->ins[0]->current_rate = FractionalNumber(60, 1);
    // D is mid-restore: outputs not yet populated.

    float error_time = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), false, error_time, 1.0f);

    REQUIRE(g.links.size() == 1);
    REQUIRE(A->route_links.size() == 1);
    REQUIRE(D->outs[0]->item == nullptr);                       // no item propagation
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(0, 1)); // no re-solve
}

/// @test An over-constrained route connection (locked supply 100 vs locked
///       demand 50) is rejected: the route link is removed and not indexed.
/// @covers GraphModel::CreateLink route-link rejection (mirrors belt links).
TEST_CASE("GraphModel::CreateLink rejects an over-constrained route", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    A->ins[0]->current_rate = FractionalNumber(100, 1);
    A->ins[0]->SetLocked(true);
    D->outs[0]->item = &rotor;
    D->outs[0]->current_rate = FractionalNumber(50, 1);
    D->outs[0]->SetLocked(true);

    float error_time = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), true, error_time, 2.0f);

    REQUIRE(g.links.empty());          // link removed
    REQUIRE(A->route_links.empty());   // unindexed on both stations
    REQUIRE(D->route_links.empty());
}

/// @test Connecting a Load station that carries an item to an Unload station
///       marks BOTH of the unloader's output belts with the item (a station's
///       cargo belts all carry its single item), but flow only lands on a belt
///       that is actually connected downstream — an idle belt stays at 0.
/// @covers GraphModel::CreateLink route cargo-item propagation (mark all belts)
///         + connection-gated rate balancing.
TEST_CASE("GraphModel::CreateLink marks both unloader belts, flows only when connected", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item plate("Iron Plate", "", 2);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &plate;
    A->ins[0]->current_rate = FractionalNumber(60, 1);
    // D's outputs start empty (no item, not connected downstream).

    float error_time = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), true, error_time, 1.0f);

    // Both output belts are marked with the routed item...
    REQUIRE(D->outs[0]->item == &plate);
    REQUIRE(D->outs[1]->item == &plate);
    // ...but neither carries flow yet (nothing connected downstream).
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(0, 1));
    REQUIRE(D->outs[1]->current_rate == FractionalNumber(0, 1));

    // Connect a sink to the first output belt: now it carries the full route
    // rate, while the still-idle second belt stays at 0.
    auto sink = std::make_unique<SinkNode>(ax::NodeEditor::NodeId(g.GetNextId()),
        [&g] { return g.GetNextId(); }, &plate);
    SinkNode* drain = sink.get();
    g.nodes.push_back(std::move(sink));
    g.CreateLink(D->outs[0].get(), drain->ins[0].get(), true, error_time, 1.0f);

    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(D->outs[1]->current_rate == FractionalNumber(0, 1));
}

/// @test Cargo-item propagation is symmetric: connecting an empty Load station
///       to an Unload station that already carries an item back-fills the loader.
/// @covers GraphModel::CreateLink route-link cargo-item propagation (demand->supply).
TEST_CASE("GraphModel::CreateLink back-fills a loader from a typed unloader", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item plate("Iron Plate", "", 2);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    D->outs[0]->item = &plate; // loader empty

    float error_time = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), true, error_time, 1.0f);

    REQUIRE(A->ins[0]->item == &plate); // back-filled onto the loader's cargo input
}

/// @test Feeding a loader's belt *after* the route is connected propagates the
///       item and rate across the route to the unloader in the same action.
/// @covers GraphModel::CreateLink belt-to-station route sync (item + re-balance).
TEST_CASE("GraphModel::CreateLink: feeding a loader belt syncs an existing route", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item plate("Iron Plate", "", 2);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    auto gen = [&g] { return g.GetNextId(); };
    // Route connected while both stations are still empty.
    float et = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), false, et, 1.0f);

    // The unloader's first output is connected downstream (to a sink), so it is
    // live and will carry the routed flow; the second output stays idle.
    auto sink = std::make_unique<SinkNode>(ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
    SinkNode* drain = sink.get();
    g.nodes.push_back(std::move(sink));
    g.CreateLink(D->outs[0].get(), drain->ins[0].get(), false, et, 1.0f);

    // Upstream source carrying 60 Iron Plate/min.
    auto storage = std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
    LogisticsNode* src = storage.get();
    g.nodes.push_back(std::move(storage));
    src->outs[0]->item = &plate;
    src->outs[0]->current_rate = FractionalNumber(60, 1);

    // Belt-feed the loader now.
    g.CreateLink(src->outs[0].get(), A->ins[0].get(), true, et, 1.0f);

    REQUIRE(A->ins[0]->item == &plate);
    REQUIRE(A->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(D->outs[0]->item == &plate);                          // crossed the route
    REQUIRE(D->outs[0]->current_rate == FractionalNumber(60, 1)); // and balanced (connected)
    REQUIRE(D->outs[1]->item == &plate);                          // both belts marked
    REQUIRE(D->outs[1]->current_rate == FractionalNumber(0, 1));  // idle belt stays at 0
}

/// @test Deleting one route link re-settles the surviving pool: after removing
///       B->D from the {A,B}->D pool, the remaining {A,D} pool rebalances so
///       supply equals demand, while the detached B keeps its rate.
/// @covers GraphModel::DeleteLink route-link branch — re-solve on disconnect.
TEST_CASE("GraphModel::DeleteLink re-settles the surviving route pool", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    Item rotor("Rotor", "", 100);

    VehicleStationNode* A = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* B = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* D = AddStation(g, VehicleStationNode::Mode::Unload);
    A->ins[0]->item = &rotor;
    B->ins[0]->item = &rotor;
    D->outs[0]->item = &rotor;
    // Balanced 3-station pool: A(20) + B(40) -> D(60).
    A->ins[0]->current_rate = FractionalNumber(20, 1);
    B->ins[0]->current_rate = FractionalNumber(40, 1);
    D->outs[0]->current_rate = FractionalNumber(60, 1);

    float et = 0.0f;
    g.CreateLink(A->plug.get(), D->plug.get(), false, et, 1.0f);
    g.CreateLink(B->plug.get(), D->plug.get(), false, et, 1.0f);
    // Find and delete the B<->D route link.
    ax::NodeEditor::LinkId bd = B->route_links[0]->id;
    g.DeleteLink(bd);

    // Surviving {A,D} pool is balanced again (supply == demand).
    auto bal = VehicleRoute::SummarizePool(VehicleRoute::FindPool(A));
    REQUIRE(bal.size() == 1);
    REQUIRE(bal[0].supply == bal[0].demand);
    // Detached B is untouched.
    REQUIRE(B->ins[0]->current_rate == FractionalNumber(40, 1));
}
