#include <catch2/catch_test_macros.hpp>

#include "domain/graph_model.hpp"
#include "graph_test_helpers.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "infra/session_serializer.hpp"

/// @test   Serializing an empty graph yields non-empty JSON, and deserializing that JSON back leaves
///         the graph empty (no spurious nodes/links invented from the envelope).
/// @covers SessionSerializer::Serialize/Deserialize on the empty boundary case — the JSON envelope
///         (version/metadata) is well-formed yet carries no graph contents.
TEST_CASE("SessionSerializer round-trips an empty graph", "[serializer]")
{
    FakeEditorBackend fake;
    GraphModel graph(fake);
    SessionSerializer serializer(graph, fake, 7);

    const std::string dumped = serializer.Serialize();

    REQUIRE_FALSE(dumped.empty());
    serializer.Deserialize(dumped);
    REQUIRE(graph.nodes.empty());
    REQUIRE(graph.links.empty());
}

/// @test   A graph with two nodes joined by a link survives a Serialize → (fresh model) Deserialize
///         round-trip: node count, link count, and the link's endpoint wiring (start on node 0,
///         end on node 1) are all reconstructed.
/// @covers SessionSerializer full round-trip into an independent GraphModel/backend, including
///         link-endpoint re-binding after deserialization. Proves saved sessions reload with
///         topology intact, not just valid JSON.
TEST_CASE("SessionSerializer round-trips a graph with nodes and links", "[serializer]")
{
    FakeEditorBackend fake;
    GraphModel source(fake);
    IdGen ids;

    auto splitter = std::make_unique<CustomSplitterNode>(source.GetNextId(), std::ref(ids));
    auto merger = std::make_unique<MergerNode>(source.GetNextId(), std::ref(ids));
    Pin* splitter_out = splitter->outs[0].get();
    Pin* merger_in = merger->ins[0].get();

    source.nodes.push_back(std::move(splitter));
    source.nodes.push_back(std::move(merger));

    float error_time = 0.0f;
    source.CreateLink(splitter_out, merger_in, false, error_time, 0.0f);

    SessionSerializer source_serializer(source, fake, 7);
    const std::string dumped = source_serializer.Serialize();

    FakeEditorBackend target_fake;
    GraphModel target(target_fake);
    SessionSerializer target_serializer(target, target_fake, 7);

    target_serializer.Deserialize(dumped);

    REQUIRE(target.nodes.size() == 2);
    REQUIRE(target.links.size() == 1);
    REQUIRE(target.links[0]->start->node == target.nodes[0].get());
    REQUIRE(target.links[0]->end->node == target.nodes[1].get());
}

/// @test   A loader and an unloader vehicle station joined by a plug<->plug route
///         link survive a Serialize -> (fresh model) Deserialize round-trip: the
///         route link is rebuilt and re-indexed on both stations' route_links,
///         and each station's mode is preserved.
/// @covers SessionSerializer route-link persistence (the route_links array) and
///         VehicleStationNode mode round-trip through the session.
TEST_CASE("SessionSerializer round-trips a vehicle route link", "[serializer][vehicle_station]")
{
    FakeEditorBackend fake;
    GraphModel source(fake);
    IdGen ids;

    auto loader = std::make_unique<VehicleStationNode>(source.GetNextId(), LogisticsNode::Kind::TruckStation, std::ref(ids));
    auto unloader = std::make_unique<VehicleStationNode>(source.GetNextId(), LogisticsNode::Kind::TruckStation, std::ref(ids));
    unloader->SetMode(VehicleStationNode::Mode::Unload, std::ref(ids));
    Pin* loader_plug = loader->plug.get();
    Pin* unloader_plug = unloader->plug.get();

    source.nodes.push_back(std::move(loader));
    source.nodes.push_back(std::move(unloader));

    float error_time = 0.0f;
    source.CreateLink(loader_plug, unloader_plug, false, error_time, 0.0f);
    REQUIRE(source.links.size() == 1); // the route link lives in graph.links too

    SessionSerializer source_serializer(source, fake, 7);
    const std::string dumped = source_serializer.Serialize();

    FakeEditorBackend target_fake;
    GraphModel target(target_fake);
    SessionSerializer target_serializer(target, target_fake, 7);
    target_serializer.Deserialize(dumped);

    REQUIRE(target.nodes.size() == 2);
    REQUIRE(target.links.size() == 1);

    auto* t_loader = static_cast<VehicleStationNode*>(target.nodes[0].get());
    auto* t_unloader = static_cast<VehicleStationNode*>(target.nodes[1].get());
    REQUIRE(t_loader->mode == VehicleStationNode::Mode::Load);
    REQUIRE(t_unloader->mode == VehicleStationNode::Mode::Unload);
    REQUIRE(t_loader->route_links.size() == 1);
    REQUIRE(t_unloader->route_links.size() == 1);
    REQUIRE(t_loader->route_links[0] == t_unloader->route_links[0]);
    REQUIRE(t_loader->route_links[0]->start == t_loader->plug.get());
    REQUIRE(t_loader->route_links[0]->end == t_unloader->plug.get());
}
