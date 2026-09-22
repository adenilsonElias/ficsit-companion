#include <catch2/catch_test_macros.hpp>

#include "domain/nodes/node.hpp"
#include "domain/nodes/node_data_resolver.hpp"
#include "domain/gamedata/recipe.hpp"
#include "domain/gamedata/building.hpp"
#include "domain/core/json.hpp"
#include "domain/graph/link.hpp"

#include "graph_test_helpers.hpp" // IdGen

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Test double for INodeDataResolver: resolves names from in-memory maps.
    class FakeNodeDataResolver : public INodeDataResolver
    {
    public:
        std::unordered_map<std::string, const Recipe*> recipes;
        std::unordered_map<std::string, const Item*> items;

        const Recipe* FindRecipe(const std::string& name) const override
        {
            auto it = recipes.find(name);
            return it == recipes.end() ? nullptr : it->second;
        }
        const Item* FindItem(const std::string& name) const override
        {
            auto it = items.find(name);
            return it == items.end() ? nullptr : it->second;
        }
    };

    // Serialize -> Deserialize(factory) -> Serialize again; the two JSON strings
    // must match, proving the deserialize path reconstructs the node faithfully.
    std::string RoundTrip(const Node& node, IdGen& idgen, const INodeDataResolver& resolver)
    {
        const Json::Value original = node.Serialize();
        std::unique_ptr<Node> rebuilt = Node::Deserialize(
            ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);
        return rebuilt->Serialize().Dump();
    }
}

/// @test   Each organizer node kind (Merger, CustomSplitter, GameSplitter) serializes, deserializes
///         via the data resolver, and re-serializes to byte-identical JSON.
/// @covers Node::Serialize/Deserialize for the three organizer kinds, including item-name → Item*
///         re-resolution through INodeDataResolver. Idempotent re-serialization proves no field is
///         lost or reordered on reload.
TEST_CASE("Organizer nodes round-trip through the resolver", "[node][serialization]")
{
    IdGen idgen;
    Item iron("Iron Ore", "", 1);
    FakeNodeDataResolver resolver;
    resolver.items["Iron Ore"] = &iron;

    MergerNode merger(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, &iron);
    CustomSplitterNode splitter(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, &iron);
    GameSplitterNode game_splitter(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, &iron);

    REQUIRE(merger.Serialize().Dump() == RoundTrip(merger, idgen, resolver));
    REQUIRE(splitter.Serialize().Dump() == RoundTrip(splitter, idgen, resolver));
    REQUIRE(game_splitter.Serialize().Dump() == RoundTrip(game_splitter, idgen, resolver));
}

/// @test   A SinkNode carrying an item round-trips to identical JSON after deserialize+re-serialize.
/// @covers Node::Serialize/Deserialize for the SinkNode kind and its item re-resolution through the
///         resolver.
TEST_CASE("Sink node round-trips through the resolver", "[node][serialization]")
{
    IdGen idgen;
    Item plate("Iron Plate", "", 2);
    FakeNodeDataResolver resolver;
    resolver.items["Iron Plate"] = &plate;

    SinkNode sink(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, &plate);
    REQUIRE(sink.Serialize().Dump() == RoundTrip(sink, idgen, resolver));
}

/// @test   A generic LogisticsNode (Storage kind, 1 in / 1 out) round-trips to identical JSON; its pins
///         carry no item so no resolver lookup is required. (TruckStation/TrainStation kinds now
///         deserialize as VehicleStationNode — covered in test_vehicle_station_node.cpp.)
/// @covers Node::Serialize/Deserialize for LogisticsNode, including its kind/port-count fields and
///         the item-less pin path through the resolver.
TEST_CASE("Logistics node round-trips", "[node][serialization]")
{
    IdGen idgen;
    FakeNodeDataResolver resolver; // pins carry no item -> "" -> no lookup needed

    LogisticsNode logi(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::Storage,
        1, 1, [&idgen] { return idgen(); });
    REQUIRE(logi.Serialize().Dump() == RoundTrip(logi, idgen, resolver));
}

/// @test   A CraftNode deserializes back to a Craft kind whose recipe pointer is rebound to the exact
///         Recipe the resolver supplies, and re-serializes to identical JSON.
/// @covers Node::Serialize/Deserialize for CraftNode plus recipe-name → Recipe* resolution: the
///         rebuilt node must reference the resolved recipe object, not a copy or a dangling pointer.
TEST_CASE("Craft node round-trips and binds the resolved recipe", "[node][serialization]")
{
    IdGen idgen;
    Building building("Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false);
    Item iron("Iron Ore", "", 1);
    Item plate("Iron Plate", "", 2);
    std::vector<CountedItem> ins{ CountedItem(&iron, FractionalNumber(30, 1)) };
    std::vector<CountedItem> outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
    Recipe recipe(ins, outs, &building, false, 4.0, "Recipe_IronPlate_C");

    FakeNodeDataResolver resolver;
    resolver.recipes["Recipe_IronPlate_C"] = &recipe;

    CraftNode craft(ax::NodeEditor::NodeId(idgen()), &recipe, [&idgen] { return idgen(); });
    const Json::Value original = craft.Serialize();
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);

    REQUIRE(rebuilt->GetKind() == Node::Kind::Craft);
    REQUIRE(static_cast<CraftNode*>(rebuilt.get())->recipe == &recipe);
    REQUIRE(craft.Serialize().Dump() == rebuilt->Serialize().Dump());
}

/// @test   A GroupNode wrapping two mergers and one internal link round-trips: the rebuilt group is a
///         Group kind with both subnodes and the internal link recreated, and re-serializes identically.
/// @covers Node::Serialize/Deserialize recursion for GroupNode — the resolver is threaded through
///         nested subnodes and sublinks, and internal topology is reconstructed, not flattened.
TEST_CASE("Group node round-trips, recursing the resolver through subnodes", "[node][serialization]")
{
    IdGen idgen;
    FakeNodeDataResolver resolver; // subnode mergers carry no item

    // Build a group of two mergers joined by one internal link.
    std::vector<std::unique_ptr<Node>> subnodes;
    subnodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, nullptr));
    subnodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, nullptr));
    MergerNode* m0 = static_cast<MergerNode*>(subnodes[0].get());
    MergerNode* m1 = static_cast<MergerNode*>(subnodes[1].get());

    std::vector<std::unique_ptr<Link>> sublinks;
    sublinks.push_back(MakeLink(idgen(), m0->outs[0].get(), m1->ins[0].get()));

    GroupNode group(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); },
        std::move(subnodes), std::move(sublinks));

    const Json::Value original = group.Serialize();
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);

    REQUIRE(rebuilt->GetKind() == Node::Kind::Group);
    GroupNode* rebuilt_group = static_cast<GroupNode*>(rebuilt.get());
    REQUIRE(rebuilt_group->nodes.size() == 2);   // subnodes recreated via the recursive resolver
    REQUIRE(rebuilt_group->links.size() == 1);   // internal link rebuilt
    REQUIRE(group.Serialize().Dump() == rebuilt->Serialize().Dump());
}

/// @test A GroupNode containing a plug<->plug vehicle route link round-trips
///       through Serialize/Deserialize with the route link preserved.
/// @covers GroupNode route-link serialization.
TEST_CASE("GroupNode round-trips vehicle route links", "[group][vehicle_route]")
{
    IdGen idgen;
    FakeNodeDataResolver resolver;

    std::vector<std::unique_ptr<Node>> subnodes;
    subnodes.push_back(std::make_unique<VehicleStationNode>(
        ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); }));
    subnodes.push_back(std::make_unique<VehicleStationNode>(
        ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); }));

    auto* loader = static_cast<VehicleStationNode*>(subnodes[0].get());
    auto* unloader = static_cast<VehicleStationNode*>(subnodes[1].get());
    unloader->SetMode(VehicleStationNode::Mode::Unload, [&idgen] { return idgen(); });

    std::vector<std::unique_ptr<Link>> sublinks;
    sublinks.emplace_back(std::make_unique<Link>(
        ax::NodeEditor::LinkId(idgen()), loader->plug.get(), unloader->plug.get()));
    Link* route_link = sublinks.back().get();
    loader->route_links.push_back(route_link);
    unloader->route_links.push_back(route_link);

    GroupNode group(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); },
        std::move(subnodes), std::move(sublinks));

    const Json::Value original = group.Serialize();
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);

    REQUIRE(rebuilt->GetKind() == Node::Kind::Group);
    auto* rebuilt_group = static_cast<GroupNode*>(rebuilt.get());
    REQUIRE(rebuilt_group->nodes.size() == 2);
    REQUIRE(rebuilt_group->links.size() == 1);

    auto* rebuilt_loader = static_cast<VehicleStationNode*>(rebuilt_group->nodes[0].get());
    auto* rebuilt_unloader = static_cast<VehicleStationNode*>(rebuilt_group->nodes[1].get());
    REQUIRE(rebuilt_loader->route_links.size() == 1);
    REQUIRE(rebuilt_unloader->route_links.size() == 1);
    REQUIRE(rebuilt_loader->route_links[0] == rebuilt_unloader->route_links[0]);
    REQUIRE(rebuilt_loader->route_links[0]->start == rebuilt_loader->plug.get());
    REQUIRE(rebuilt_loader->route_links[0]->end == rebuilt_unloader->plug.get());
    REQUIRE(rebuilt_loader->plug->links.empty());
    REQUIRE(rebuilt_unloader->plug->links.empty());
}

/// @test   An ExtractorNode (MinerMk2, Pure purity, extracting an item) round-trips to identical JSON.
/// @covers Node::Serialize/Deserialize for ExtractorNode — its kind, purity, and extracted-item
///         fields plus item re-resolution.
TEST_CASE("Extractor node round-trips through the resolver", "[node][serialization]")
{
    IdGen idgen;
    Item ore("Iron Ore", "", 1);
    FakeNodeDataResolver resolver;
    resolver.items["Iron Ore"] = &ore;

    ExtractorNode extractor(ax::NodeEditor::NodeId(idgen()), ExtractorNode::Kind::MinerMk2,
        &ore, ExtractorNode::Purity::Pure, [&idgen] { return idgen(); });
    REQUIRE(extractor.Serialize().Dump() == RoundTrip(extractor, idgen, resolver));
}

/// @test   The deserialization factory routes a known kind tag to the right node type (a serialized
///         merger rebuilds as Kind::Merger) and throws on an unknown kind tag (99).
/// @covers Node::Deserialize's kind-dispatch switch and its unknown-kind guard — preventing a
///         corrupt or future-version "kind" value from silently producing the wrong node or null.
TEST_CASE("Factory dispatches each kind and rejects unknown kinds", "[node][serialization]")
{
    IdGen idgen;
    Item iron("Iron Ore", "", 1);
    FakeNodeDataResolver resolver;
    resolver.items["Iron Ore"] = &iron;

    MergerNode merger(ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, &iron);
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, merger.Serialize(), resolver);
    REQUIRE(rebuilt->GetKind() == Node::Kind::Merger);

    Json::Value bogus;
    bogus["kind"] = 99;
    REQUIRE_THROWS(Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, bogus, resolver));
}

/// @test   Deserializing a CraftNode whose recipe name isn't registered in the resolver throws,
///         rather than producing a craft with a null/dangling recipe.
/// @covers Node::Deserialize's failed-resolution guard for recipes — a save referencing an unknown
///         recipe (e.g. removed/renamed in game data) fails loudly instead of corrupting the graph.
TEST_CASE("Deserialize rejects an unresolvable recipe", "[node][serialization]")
{
    IdGen idgen;
    Building building("Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false);
    Item iron("Iron Ore", "", 1);
    Item plate("Iron Plate", "", 2);
    std::vector<CountedItem> ins{ CountedItem(&iron, FractionalNumber(30, 1)) };
    std::vector<CountedItem> outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
    Recipe recipe(ins, outs, &building, false, 4.0, "Recipe_IronPlate_C");

    CraftNode craft(ax::NodeEditor::NodeId(idgen()), &recipe, [&idgen] { return idgen(); });
    FakeNodeDataResolver empty; // no recipes registered
    REQUIRE_THROWS(Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, craft.Serialize(), empty));
}
