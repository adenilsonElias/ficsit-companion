#include <catch2/catch_test_macros.hpp>

#include "domain/gamedata/building.hpp"
#include "domain/graph/graph_model.hpp"
#include "graph_test_helpers.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp" // Item
#include "domain/solver/rate_solver.hpp"
#include "domain/vehicle/vehicle_route.hpp"

#include <stdexcept>

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

    struct ThrowingNode final : Node
    {
        ThrowingNode(ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator)
            : Node(id)
        {
            outs.emplace_back(std::make_unique<Pin>(
                id_generator(), ax::NodeEditor::PinKind::Output, this, nullptr));
        }

        Kind GetKind() const override
        {
            throw std::runtime_error("synthetic propagation failure");
        }
    };
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

TEST_CASE("GraphModel::GetNextId resumes from a restored id", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    g.next_id = 41;

    REQUIRE(g.GetNextId() == 41);
    REQUIRE(g.GetNextId() == 42);
}

TEST_CASE("GraphModel::FindPin finds exposed pins and excludes nested group pins", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };
    Item plate("Synthetic Plate", "", 2);

    auto splitter = std::make_unique<CustomSplitterNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
    Pin* splitter_in = splitter->ins[0].get();
    Pin* splitter_out = splitter->outs[2].get();
    g.nodes.push_back(std::move(splitter));

    auto station = std::make_unique<VehicleStationNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::TrainStation, gen);
    Pin* station_plug = station->plug.get();
    g.nodes.push_back(std::move(station));

    std::vector<std::unique_ptr<Node>> nested_nodes;
    auto nested_sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
    nested_sink->ins[0]->current_rate = FractionalNumber(30, 1);
    const ax::NodeEditor::PinId nested_pin_id = nested_sink->ins[0]->id;
    nested_nodes.push_back(std::move(nested_sink));
    auto group = std::make_unique<GroupNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen, std::move(nested_nodes),
        std::vector<std::unique_ptr<Link>>{});
    REQUIRE(group->ins.size() == 1);
    Pin* group_exposed_in = group->ins[0].get();
    g.nodes.push_back(std::move(group));

    REQUIRE(g.FindPin(ax::NodeEditor::PinId::Invalid) == nullptr);
    REQUIRE(g.FindPin(splitter_in->id) == splitter_in);
    REQUIRE(g.FindPin(splitter_out->id) == splitter_out);
    REQUIRE(g.FindPin(station_plug->id) == station_plug);
    REQUIRE(g.FindPin(group_exposed_in->id) == group_exposed_in);
    REQUIRE(g.FindPin(nested_pin_id) == nullptr);
    REQUIRE(g.FindPin(ax::NodeEditor::PinId(999999)) == nullptr);
}

TEST_CASE("GraphModel::CreateLink restore mode normalizes direction without solving", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };
    Item plate("Synthetic Plate", "", 2);

    auto source = std::make_unique<CustomSplitterNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    Pin* source_out = source->outs[0].get();
    Pin* sink_in = sink->ins[0].get();
    source_out->current_rate = FractionalNumber(60, 1);
    sink_in->current_rate = FractionalNumber(10, 1);
    source_out->SetLocked(true);
    g.nodes.push_back(std::move(source));
    g.nodes.push_back(std::move(sink));

    float error_time = 7.0f;
    g.CreateLink(sink_in, source_out, false, error_time, 3.0f);

    REQUIRE(g.links.size() == 1);
    REQUIRE(g.links[0]->start == source_out);
    REQUIRE(g.links[0]->end == sink_in);
    REQUIRE(source_out->SoleLink() == g.links[0].get());
    REQUIRE(sink_in->SoleLink() == g.links[0].get());
    REQUIRE(source_out->current_rate == FractionalNumber(60, 1));
    REQUIRE(sink_in->current_rate == FractionalNumber(10, 1));
    REQUIRE(source_out->GetLocked());
    REQUIRE(sink_in->GetLocked());
    REQUIRE(sink_in->item == &plate);
    REQUIRE(error_time == 7.0f);
}

TEST_CASE("GraphModel::CreateLink removes a rejected ordinary link", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };

    auto source = std::make_unique<MergerNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    Pin* source_out = source->outs[0].get();
    Pin* locked_input = source->ins[0].get();
    Pin* sink_in = sink->ins[0].get();
    source_out->current_rate = FractionalNumber(10, 1);
    locked_input->current_rate = FractionalNumber(100, 1);
    locked_input->SetLocked(true);
    g.nodes.push_back(std::move(source));
    g.nodes.push_back(std::move(sink));

    float error_time = 0.0f;
    g.CreateLink(source_out, sink_in, true, error_time, 2.0f);

    REQUIRE(g.links.empty());
    REQUIRE(source_out->links.empty());
    REQUIRE(sink_in->links.empty());
    REQUIRE(fake.deleted_links.size() == 1);
    REQUIRE(error_time == 2.0f);
    REQUIRE(locked_input->current_rate == FractionalNumber(100, 1));
}

TEST_CASE("GraphModel::CreateLink cleans up after a propagation exception", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };

    auto source = std::make_unique<ThrowingNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    Pin* source_out = source->outs[0].get();
    Pin* sink_in = sink->ins[0].get();
    source_out->current_rate = FractionalNumber(10, 1);
    g.nodes.push_back(std::move(source));
    g.nodes.push_back(std::move(sink));

    float error_time = 0.0f;
    g.CreateLink(source_out, sink_in, true, error_time, 2.5f);

    REQUIRE(g.links.empty());
    REQUIRE(source_out->links.empty());
    REQUIRE(sink_in->links.empty());
    REQUIRE(fake.deleted_links.size() == 1);
    REQUIRE(error_time == 2.5f);
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
    REQUIRE(target_in->links.empty());
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

TEST_CASE("GraphModel::DeleteLink reports a miss without mutating the graph", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };
    auto storage = std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
    g.nodes.push_back(std::move(storage));

    g.DeleteLink(ax::NodeEditor::LinkId(700));

    REQUIRE(g.nodes.size() == 1);
    REQUIRE(g.links.empty());
    REQUIRE(fake.deleted_links == std::vector<ax::NodeEditor::LinkId>{ax::NodeEditor::LinkId(700)});
}

TEST_CASE("GraphModel::DeleteLink clears endpoint-specific state", "[graph_model]")
{
    Item plate("Synthetic Plate", "", 2);

    SECTION("logistics source and sink destination")
    {
        FakeEditorBackend fake;
        GraphModel g(fake);
        auto gen = [&g] { return g.GetNextId(); };
        auto source = std::make_unique<LogisticsNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
        auto sink = std::make_unique<SinkNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
        Pin* source_out = source->outs[0].get();
        Pin* sink_in = sink->ins[0].get();
        source_out->item = &plate;
        source_out->current_rate = FractionalNumber(60, 1);
        sink_in->current_rate = FractionalNumber(60, 1);
        g.nodes.push_back(std::move(source));
        g.nodes.push_back(std::move(sink));
        g.links.push_back(MakeLink(701, source_out, sink_in));

        g.DeleteLink(ax::NodeEditor::LinkId(701));

        REQUIRE(source_out->links.empty());
        REQUIRE(source_out->item == nullptr);
        REQUIRE(source_out->current_rate == FractionalNumber(0, 1));
        REQUIRE(sink_in->links.empty());
        REQUIRE(sink_in->item == nullptr);
        REQUIRE(sink_in->current_rate == FractionalNumber(0, 1));
        REQUIRE(g.links.empty());
    }

    SECTION("organizer endpoints drop an unforced item")
    {
        FakeEditorBackend fake;
        GraphModel g(fake);
        auto gen = [&g] { return g.GetNextId(); };
        auto splitter = std::make_unique<CustomSplitterNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
        auto merger = std::make_unique<MergerNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
        CustomSplitterNode* splitter_raw = splitter.get();
        MergerNode* merger_raw = merger.get();
        Pin* source_out = splitter->outs[0].get();
        Pin* merger_in = merger->ins[0].get();
        g.nodes.push_back(std::move(splitter));
        g.nodes.push_back(std::move(merger));
        g.links.push_back(MakeLink(702, source_out, merger_in));

        g.DeleteLink(ax::NodeEditor::LinkId(702));

        REQUIRE(splitter_raw->item == nullptr);
        REQUIRE(merger_raw->item == nullptr);
        REQUIRE(source_out->links.empty());
        REQUIRE(merger_in->links.empty());
    }

    SECTION("logistics destination clears its input")
    {
        FakeEditorBackend fake;
        GraphModel g(fake);
        auto gen = [&g] { return g.GetNextId(); };
        auto splitter = std::make_unique<CustomSplitterNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), gen, &plate);
        auto storage = std::make_unique<LogisticsNode>(
            ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
        Pin* source_out = splitter->outs[0].get();
        Pin* storage_in = storage->ins[0].get();
        storage_in->item = &plate;
        storage_in->current_rate = FractionalNumber(25, 1);
        g.nodes.push_back(std::move(splitter));
        g.nodes.push_back(std::move(storage));
        g.links.push_back(MakeLink(703, source_out, storage_in));

        g.DeleteLink(ax::NodeEditor::LinkId(703));

        REQUIRE(storage_in->links.empty());
        REQUIRE(storage_in->item == nullptr);
        REQUIRE(storage_in->current_rate == FractionalNumber(0, 1));
    }
}

TEST_CASE("GraphModel::DeleteNode reports a miss to the backend", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };
    auto storage = std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
    const ax::NodeEditor::NodeId existing_id = storage->id;
    g.nodes.push_back(std::move(storage));

    g.DeleteNode(ax::NodeEditor::NodeId(800));

    REQUIRE(g.nodes.size() == 1);
    REQUIRE(g.nodes[0]->id == existing_id);
    REQUIRE(fake.deleted_nodes == std::vector<ax::NodeEditor::NodeId>{ax::NodeEditor::NodeId(800)});
    REQUIRE(fake.deleted_links.empty());
}

TEST_CASE("GraphModel::DeleteNode removes incoming and outgoing links", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto gen = [&g] { return g.GetNextId(); };

    auto source = std::make_unique<CustomSplitterNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    auto storage = std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), LogisticsNode::Kind::Storage, 1, 1, gen);
    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(g.GetNextId()), gen);
    Pin* source_out = source->outs[0].get();
    Pin* storage_in = storage->ins[0].get();
    Pin* storage_out = storage->outs[0].get();
    Pin* sink_in = sink->ins[0].get();
    const ax::NodeEditor::NodeId storage_id = storage->id;
    g.nodes.push_back(std::move(source));
    g.nodes.push_back(std::move(storage));
    g.nodes.push_back(std::move(sink));
    g.links.push_back(MakeLink(801, source_out, storage_in));
    g.links.push_back(MakeLink(802, storage_out, sink_in));

    g.DeleteNode(storage_id);

    REQUIRE(g.nodes.size() == 2);
    REQUIRE(g.links.empty());
    REQUIRE(source_out->links.empty());
    REQUIRE(sink_in->links.empty());
    REQUIRE(fake.deleted_nodes == std::vector<ax::NodeEditor::NodeId>{storage_id});
    REQUIRE(fake.deleted_links == std::vector<ax::NodeEditor::LinkId>{
        ax::NodeEditor::LinkId(801), ax::NodeEditor::LinkId(802)});
}

TEST_CASE("GraphModel::DeleteNode removes every vehicle route link", "[graph_model][vehicle_route]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    VehicleStationNode* loader = AddStation(g, VehicleStationNode::Mode::Load);
    VehicleStationNode* first_unloader = AddStation(g, VehicleStationNode::Mode::Unload);
    VehicleStationNode* second_unloader = AddStation(g, VehicleStationNode::Mode::Unload);
    float error_time = 0.0f;
    g.CreateLink(loader->plug.get(), first_unloader->plug.get(), false, error_time, 1.0f);
    g.CreateLink(loader->plug.get(), second_unloader->plug.get(), false, error_time, 1.0f);
    const ax::NodeEditor::NodeId loader_id = loader->id;
    const ax::NodeEditor::LinkId first_link_id = loader->route_links[0]->id;
    const ax::NodeEditor::LinkId second_link_id = loader->route_links[1]->id;

    g.DeleteNode(loader_id);

    REQUIRE(g.nodes.size() == 2);
    REQUIRE(g.links.empty());
    REQUIRE(first_unloader->route_links.empty());
    REQUIRE(second_unloader->route_links.empty());
    REQUIRE(fake.deleted_nodes == std::vector<ax::NodeEditor::NodeId>{loader_id});
    REQUIRE(fake.deleted_links == std::vector<ax::NodeEditor::LinkId>{
        first_link_id, second_link_id});
}

// ---------------------------------------------------------------------------
// Multi-link pins: wiring a second consumer onto an output pin that already has
// one. Which end drives the solve is what decides whether production ramps up or
// the new consumer gets squeezed.
// ---------------------------------------------------------------------------

namespace
{
    // A 30 ore -> 20 plate producer, and a 20 plate -> 10 rod consumer.
    struct FanOutRecipes
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

/// @test   Wiring a new consumer onto an output pin that already has a link makes production ramp
///         up to the summed demand: the new consumer keeps the rate it was configured with, and
///         the consumer that was already there is not squeezed.
/// @covers The constraint-pin choice in GraphModel::CreateLink. This is the step that turns the
///         multi-link solver into the behaviour the user sees. CreateLink normally pushes from
///         the source; doing that here would drag the new consumer to whatever rate the source
///         already carried (40), instead of letting it keep its own demand (20) and raising the
///         producer to 60.
TEST_CASE("CreateLink on a fan-out pulls from the newly wired end", "[graph_model][multilink]")
{
    FanOutRecipes fx;
    FakeEditorBackend fake;
    GraphModel g(fake);
    auto id_gen = [&g] { return g.GetNextId(); };

    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.producer, id_gen));
    CraftNode* producer = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* screw = static_cast<CraftNode*>(g.nodes.back().get());
    g.nodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(id_gen()), &fx.consumer, id_gen));
    CraftNode* rotor = static_cast<CraftNode*>(g.nodes.back().get());

    float error_time = 0.0f;

    // Established: the producer feeds the screw at 40/min.
    g.CreateLink(producer->outs[0].get(), screw->ins[0].get(), true, error_time, 1.0f);
    REQUIRE(RateSolver::Solve(g.nodes, g.links, screw->ins[0].get(),
                              FractionalNumber(40, 1), error_time, 1.0f));
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(40, 1));

    // The rotor already demands 20/min before being wired up.
    REQUIRE(RateSolver::Solve(g.nodes, g.links, rotor->ins[0].get(),
                              FractionalNumber(20, 1), error_time, 1.0f));
    REQUIRE(rotor->ins[0]->current_rate == FractionalNumber(20, 1));

    // Second link on the SAME output pin.
    g.CreateLink(producer->outs[0].get(), rotor->ins[0].get(), true, error_time, 1.0f);

    REQUIRE(error_time == 0.0f);
    REQUIRE(g.links.size() == 2); // the link survived; a rejected solve would have deleted it
    REQUIRE(producer->outs[0]->links.size() == 2);
    REQUIRE(producer->outs[0]->current_rate == FractionalNumber(60, 1)); // ramped up: 40 + 20
    REQUIRE(screw->ins[0]->current_rate == FractionalNumber(40, 1));     // not squeezed
    REQUIRE(rotor->ins[0]->current_rate == FractionalNumber(20, 1));     // kept its demand
    REQUIRE(producer->ins[0]->current_rate == FractionalNumber(90, 1));  // 60 plate needs 90 ore
}
