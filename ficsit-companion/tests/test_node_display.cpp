#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <vector>

#include "domain/nodes/node.hpp"
#include "domain/nodes/node_display.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp" // Item

namespace
{
    unsigned long long g_counter = 1;
    unsigned long long Gen() { return g_counter++; }
}

/// @test NodeDisplayName labels the organizer and sink kinds with fixed names
/// (these node kinds construct without game data or an editor context).
/// @covers NodeDisplayName for Merger / CustomSplitter / GameSplitter / Sink.
TEST_CASE("NodeDisplayName labels organizer and sink kinds", "[node_display]")
{
    MergerNode merger(1, Gen);
    REQUIRE(NodeDisplayName(merger) == "Merger");

    CustomSplitterNode custom_splitter(2, Gen);
    REQUIRE(NodeDisplayName(custom_splitter) == "Splitter");

    GameSplitterNode game_splitter(3, Gen);
    REQUIRE(NodeDisplayName(game_splitter) == "Splitter");

    SinkNode sink(4, Gen);
    REQUIRE(NodeDisplayName(sink) == "AWESOME Sink");
}

/// @test NodePrimaryItem returns the node's representative item: the first output
/// pin item, else the first input pin item, else null. Used to pick the big
/// per-node "product" icon in the read-only snapshot graph.
/// @covers NodePrimaryItem output path, input path, and the no-item case.
TEST_CASE("NodePrimaryItem picks the representative item", "[node_display]")
{
    const Item iron("Iron Ingot", "", 0);

    // Output path: a custom splitter carries the item on its output pins.
    CustomSplitterNode splitter(10, Gen, &iron);
    REQUIRE(NodePrimaryItem(splitter) == &iron);

    // Input path: a sink has only an input pin carrying the item.
    SinkNode sink_with_item(11, Gen, &iron);
    REQUIRE(NodePrimaryItem(sink_with_item) == &iron);

    // No-item case: a sink built without an item returns null.
    SinkNode sink_empty(12, Gen);
    REQUIRE(NodePrimaryItem(sink_empty) == nullptr);
}

/// @test NodeDisplayName appends the purity for extractors that support it
/// (miners, oil) and omits it for the Water Extractor. Asserts on the title's
/// parenthetical so it holds regardless of whether game data is loaded.
/// @covers NodeDisplayName extractor purity branch + ExtractorNode::SupportsPurity.
TEST_CASE("NodeDisplayName shows miner purity but not water", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);
    ExtractorNode miner(20, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Pure, Gen);
    REQUIRE(NodeDisplayName(miner).find("(Iron Ore, Pure)") != std::string::npos);

    const Item water("Water", "", 0);
    ExtractorNode extractor(21, ExtractorNode::Kind::WaterExtractor, &water, ExtractorNode::Purity::Normal, Gen);
    const std::string water_title = NodeDisplayName(extractor);
    REQUIRE(water_title.find("(Water)") != std::string::npos);
    REQUIRE(water_title.find("Normal") == std::string::npos);
}

/// @test NodesProducingItem returns only Craft/Extractor nodes that output the
/// named item; pass-through nodes (mergers) that merely carry it are excluded.
/// @covers NodesProducingItem matching + producer filtering.
TEST_CASE("NodesProducingItem returns only producers of the item", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);

    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::make_unique<ExtractorNode>(30, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Normal, Gen));
    nodes.push_back(std::make_unique<MergerNode>(31, Gen, &iron)); // carries iron on its output, but is not a producer

    const Node* expected_producer = nodes[0].get();

    const std::vector<const Node*> producers = NodesProducingItem(nodes, "Iron Ore");
    REQUIRE(producers.size() == 1);
    REQUIRE(producers[0] == expected_producer);

    REQUIRE(NodesProducingItem(nodes, "Copper Ore").empty());
}

/// @test NodesConsumingItem returns only Craft/Sink nodes that take the named
/// item on an input pin; pass-through nodes (mergers) and producers (extractors
/// that only output it) are excluded.
/// @covers NodesConsumingItem matching + consumer filtering.
TEST_CASE("NodesConsumingItem returns only consumers of the item", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);

    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::make_unique<SinkNode>(50, Gen, &iron));     // consumes iron (input pin)
    nodes.push_back(std::make_unique<MergerNode>(51, Gen, &iron));   // carries iron on input, but is pass-through
    nodes.push_back(std::make_unique<ExtractorNode>(52, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Normal, Gen)); // only outputs iron

    const Node* expected_consumer = nodes[0].get();

    const std::vector<const Node*> consumers = NodesConsumingItem(nodes, "Iron Ore");
    REQUIRE(consumers.size() == 1);
    REQUIRE(consumers[0] == expected_consumer);

    REQUIRE(NodesConsumingItem(nodes, "Copper Ore").empty());
}

/// @test NodeProductionHidden hides a single-output producer when its item is
/// in the hidden set, keeps it otherwise, and never hides non-producers.
/// @covers NodeProductionHidden producer/non-producer + hidden-set membership.
TEST_CASE("NodeProductionHidden hides producers whose every output is hidden", "[node_display]")
{
    const Item iron_ore("Iron Ore", "", 0);

    // Single-output producer (a miner).
    ExtractorNode miner(60, ExtractorNode::Kind::MinerMk1, &iron_ore, ExtractorNode::Purity::Normal, Gen);

    REQUIRE(NodeProductionHidden(miner, { "Iron Ore" }) == true);
    REQUIRE(NodeProductionHidden(miner, { "Copper Ore" }) == false);
    REQUIRE(NodeProductionHidden(miner, {}) == false);

    // A non-producer (merger) is never hidden by this predicate, even if it
    // carries the item.
    MergerNode merger(61, Gen, &iron_ore);
    REQUIRE(NodeProductionHidden(merger, { "Iron Ore" }) == false);
}

/// @test A multi-output producer stays visible while any one output item is
/// still visible, and hides only once all of its outputs are hidden.
/// @covers NodeProductionHidden "all outputs hidden" rule.
TEST_CASE("NodeProductionHidden keeps multi-output producers with a visible output", "[node_display]")
{
    const Item iron_ingot("Iron Ingot", "", 0);
    const Item slag("Slag", "", 0);

    // Build a producer with two distinct output items: a miner (1 output) plus a
    // second manually-added output pin. ExtractorNode IsExtractor() => producer.
    ExtractorNode producer(70, ExtractorNode::Kind::MinerMk1, &iron_ingot, ExtractorNode::Purity::Normal, Gen);
    producer.outs.push_back(std::make_unique<Pin>(
        ax::NodeEditor::PinId(Gen()), ax::NodeEditor::PinKind::Output, &producer, &slag));

    REQUIRE(NodeProductionHidden(producer, { "Iron Ingot" }) == false);          // Slag still visible
    REQUIRE(NodeProductionHidden(producer, { "Iron Ingot", "Slag" }) == true);   // all hidden
}

/// @test NodeSnapshotCategory maps each node kind (and logistics sub-kind) to its
/// coarse visual category, used to color snapshot nodes so categories are
/// distinguishable at any zoom.
/// @covers NodeSnapshotCategory for Production / Flow / Storage / Station / Sink.
TEST_CASE("NodeSnapshotCategory classifies nodes by category", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);

    ExtractorNode miner(40, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Normal, Gen);
    REQUIRE(NodeSnapshotCategory(miner) == SnapshotCategory::Production);

    MergerNode merger(41, Gen);
    REQUIRE(NodeSnapshotCategory(merger) == SnapshotCategory::Flow);

    CustomSplitterNode splitter(42, Gen);
    REQUIRE(NodeSnapshotCategory(splitter) == SnapshotCategory::Flow);

    LogisticsNode storage(43, LogisticsNode::Kind::Storage, 1, 1, Gen);
    REQUIRE(NodeSnapshotCategory(storage) == SnapshotCategory::Storage);

    LogisticsNode pipe(44, LogisticsNode::Kind::PipeJunction, 1, 1, Gen);
    REQUIRE(NodeSnapshotCategory(pipe) == SnapshotCategory::Flow);

    VehicleStationNode station(45, LogisticsNode::Kind::TrainStation, Gen);
    REQUIRE(NodeSnapshotCategory(station) == SnapshotCategory::Station);

    SinkNode sink(46, Gen);
    REQUIRE(NodeSnapshotCategory(sink) == SnapshotCategory::Sink);
}
