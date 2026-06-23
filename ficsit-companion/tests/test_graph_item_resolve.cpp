#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "domain/gamedata/building.hpp"
#include "domain/graph/graph_item_resolve.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include "graph_test_helpers.hpp"

namespace
{
    struct ResolveFixture
    {
        IdGen ids;
        Building building{ "GraphResolveBuilding", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Graph Resolve Ore", "", 1 };
        Item ingot{ "Graph Resolve Ingot", "", 2 };
        Item fuel{ "Graph Resolve Fuel", "", 3 };
        Item other{ "Graph Resolve Other", "", 4 };
        std::vector<CountedItem> recipe_inputs{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> recipe_outputs{ CountedItem(&ingot, FractionalNumber(30, 1)) };
        Recipe recipe{ recipe_inputs, recipe_outputs, &building, false, 4.0,
                       "Recipe_GraphResolve_C" };

        auto Generator()
        {
            return [this] { return ids(); };
        }

        std::unique_ptr<Link> LinkPins(Pin* output, Pin* input)
        {
            return MakeLink(ids(), output, input);
        }
    };
}

TEST_CASE("pin role helpers classify station fuel plug and active cargo pins",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();

    Pin orphan(ax::NodeEditor::PinId(fx.ids()), ax::NodeEditor::PinKind::Input,
               nullptr, nullptr);
    MergerNode merger(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode storage(ax::NodeEditor::NodeId(fx.ids()),
                          LogisticsNode::Kind::Storage, 1, 1, gen);
    VehicleStationNode truck(ax::NodeEditor::NodeId(fx.ids()),
                             LogisticsNode::Kind::TruckStation, gen);

    REQUIRE_FALSE(IsFuelPin(nullptr));
    REQUIRE_FALSE(IsFuelPin(&orphan));
    REQUIRE_FALSE(IsFuelPin(merger.ins[0].get()));
    REQUIRE_FALSE(IsFuelPin(storage.ins[0].get()));
    REQUIRE(IsFuelPin(truck.ins.back().get()));
    REQUIRE_FALSE(IsFuelPin(truck.ins[0].get()));

    REQUIRE_FALSE(IsVehiclePlug(nullptr));
    REQUIRE_FALSE(IsVehiclePlug(&orphan));
    REQUIRE_FALSE(IsVehiclePlug(storage.outs[0].get()));
    REQUIRE(IsVehiclePlug(truck.plug.get()));
    REQUIRE_FALSE(IsVehiclePlug(truck.ins[0].get()));

    REQUIRE(IsActiveCargoPin(truck.ins[0].get()));
    REQUIRE_FALSE(IsActiveCargoPin(truck.ins.back().get()));
    REQUIRE_FALSE(IsActiveCargoPin(truck.outs[0].get()));
    REQUIRE_FALSE(IsActiveCargoPin(truck.plug.get()));
    REQUIRE_FALSE(IsActiveCargoPin(nullptr));
    REQUIRE_FALSE(IsActiveCargoPin(&orphan));
    REQUIRE_FALSE(IsActiveCargoPin(storage.ins[0].get()));

    truck.SetMode(VehicleStationNode::Mode::Unload, gen);
    REQUIRE_FALSE(IsActiveCargoPin(truck.ins[0].get()));
    REQUIRE(IsActiveCargoPin(truck.outs[0].get()));

    VehicleStationNode train(ax::NodeEditor::NodeId(fx.ids()),
                             LogisticsNode::Kind::TrainStation,
                             VehicleStationNode::Mode::Unload, 1, 1, gen);
    REQUIRE(IsFuelPin(train.ins.back().get()));
    REQUIRE(IsVehiclePlug(train.plug.get()));
    REQUIRE(IsActiveCargoPin(train.outs[0].get()));

    auto detached_input = std::move(truck.ins.back());
    truck.ins.clear();
    REQUIRE_FALSE(IsFuelPin(detached_input.get()));
}

TEST_CASE("ResolveItemThroughChain handles typed pins organizer hops and dead ends",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();

    Pin typed(ax::NodeEditor::PinId(fx.ids()), ax::NodeEditor::PinKind::Input,
              nullptr, &fx.other);
    Pin untyped(ax::NodeEditor::PinId(fx.ids()), ax::NodeEditor::PinKind::Input,
                nullptr, nullptr);
    REQUIRE(ResolveItemThroughChain(nullptr) == nullptr);
    REQUIRE(ResolveItemThroughChain(&typed) == &fx.other);
    REQUIRE(ResolveItemThroughChain(&untyped) == nullptr);

    MergerNode origin(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode storage(ax::NodeEditor::NodeId(fx.ids()),
                          LogisticsNode::Kind::Storage, 1, 1, gen);
    CraftNode consumer(ax::NodeEditor::NodeId(fx.ids()), &fx.recipe, gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(origin.outs[0].get(), storage.ins[0].get()));
    links.push_back(fx.LinkPins(storage.outs[0].get(), consumer.ins[0].get()));

    REQUIRE(ResolveItemThroughChain(origin.ins[0].get()) == &fx.ore);

    storage.outs[0]->item = &fx.ingot;
    REQUIRE(ResolveItemThroughChain(storage.ins[0].get()) == &fx.ingot);

    MergerNode dead_end(ax::NodeEditor::NodeId(fx.ids()), gen);
    REQUIRE(ResolveItemThroughChain(dead_end.ins[0].get()) == nullptr);

    SinkNode untyped_sink(ax::NodeEditor::NodeId(fx.ids()), gen, nullptr);
    REQUIRE(ResolveItemThroughChain(untyped_sink.ins[0].get()) == nullptr);
}

TEST_CASE("ResolveItemThroughChain terminates on an untyped organizer cycle",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    MergerNode first(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode second(ax::NodeEditor::NodeId(fx.ids()),
                         LogisticsNode::Kind::Storage, 1, 1, gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(first.outs[0].get(), second.ins[0].get()));
    links.push_back(fx.LinkPins(second.outs[0].get(), first.ins[0].get()));

    REQUIRE(ResolveItemThroughChain(first.ins[1].get()) == nullptr);
}

TEST_CASE("ResolveOrganizerItem finds producer and consumer items through chains",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    CraftNode craft(ax::NodeEditor::NodeId(fx.ids()), &fx.recipe, gen);
    MergerNode from_producer(ax::NodeEditor::NodeId(fx.ids()), gen);
    std::vector<std::unique_ptr<Link>> producer_links;
    producer_links.push_back(
        fx.LinkPins(craft.outs[0].get(), from_producer.ins[0].get()));

    REQUIRE(ResolveOrganizerItem(nullptr) == nullptr);
    REQUIRE(ResolveOrganizerItem(&from_producer) == &fx.ingot);

    MergerNode toward_consumer(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode storage(ax::NodeEditor::NodeId(fx.ids()),
                          LogisticsNode::Kind::Storage, 1, 1, gen);
    std::vector<std::unique_ptr<Link>> consumer_links;
    consumer_links.push_back(
        fx.LinkPins(toward_consumer.outs[0].get(), storage.ins[0].get()));
    consumer_links.push_back(
        fx.LinkPins(storage.outs[0].get(), craft.ins[0].get()));

    REQUIRE(ResolveOrganizerItem(&toward_consumer) == &fx.ore);
}

TEST_CASE("ResolveOrganizerItem accepts extractor producers and ignores station fuel",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    ExtractorNode extractor(ax::NodeEditor::NodeId(fx.ids()),
                            ExtractorNode::Kind::MinerMk1, &fx.ore,
                            ExtractorNode::Purity::Normal, gen);
    MergerNode merger(ax::NodeEditor::NodeId(fx.ids()), gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(extractor.outs[0].get(), merger.ins[0].get()));
    REQUIRE(ResolveOrganizerItem(&merger) == &fx.ore);

    ExtractorNode fuel_source(ax::NodeEditor::NodeId(fx.ids()),
                              ExtractorNode::Kind::MinerMk1, &fx.fuel,
                              ExtractorNode::Purity::Normal, gen);
    VehicleStationNode station(ax::NodeEditor::NodeId(fx.ids()),
                               LogisticsNode::Kind::TruckStation, gen);
    links.push_back(
        fx.LinkPins(fuel_source.outs[0].get(), station.ins.back().get()));
    REQUIRE(ResolveOrganizerItem(&station) == nullptr);
}

TEST_CASE("ResolveOrganizerItem terminates on organizer and logistics cycles",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    MergerNode first(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode second(ax::NodeEditor::NodeId(fx.ids()),
                         LogisticsNode::Kind::Storage, 1, 1, gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(first.outs[0].get(), second.ins[0].get()));
    links.push_back(fx.LinkPins(second.outs[0].get(), first.ins[0].get()));

    REQUIRE(ResolveOrganizerItem(&first) == nullptr);
}

TEST_CASE("RecalculateOrganizerItemChain relabels cargo chain but preserves walls and fuel",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    CraftNode producer(ax::NodeEditor::NodeId(fx.ids()), &fx.recipe, gen);
    MergerNode origin(ax::NodeEditor::NodeId(fx.ids()), gen);
    VehicleStationNode station(ax::NodeEditor::NodeId(fx.ids()),
                               LogisticsNode::Kind::TruckStation, gen);
    GameSplitterNode game_splitter(ax::NodeEditor::NodeId(fx.ids()), gen);
    CustomSplitterNode wall(ax::NodeEditor::NodeId(fx.ids()), gen);
    MergerNode beyond_wall(ax::NodeEditor::NodeId(fx.ids()), gen);
    station.ins.back()->item = &fx.fuel;

    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(producer.outs[0].get(), origin.ins[0].get()));
    links.push_back(fx.LinkPins(origin.outs[0].get(), station.ins[0].get()));
    links.push_back(
        fx.LinkPins(station.outs[0].get(), game_splitter.ins[0].get()));
    links.push_back(
        fx.LinkPins(game_splitter.outs[0].get(), wall.ins[0].get()));
    links.push_back(
        fx.LinkPins(wall.outs[0].get(), beyond_wall.ins[0].get()));

    RecalculateOrganizerItemChain(&origin);

    REQUIRE(origin.item == &fx.ingot);
    REQUIRE(game_splitter.item == &fx.ingot);
    REQUIRE(station.ins[0]->item == &fx.ingot);
    REQUIRE(station.outs[0]->item == &fx.ingot);
    REQUIRE(station.ins.back()->item == &fx.fuel);
    REQUIRE(wall.item == nullptr);
    REQUIRE(beyond_wall.item == nullptr);

    RecalculateOrganizerItemChain(nullptr);
    MergerNode unresolved(ax::NodeEditor::NodeId(fx.ids()), gen, &fx.other);
    RecalculateOrganizerItemChain(&unresolved);
    REQUIRE(unresolved.item == &fx.other);
}

TEST_CASE("PropagateExtractorResourceUpstream fills missing resources through graph nodes",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    ExtractorNode extractor(ax::NodeEditor::NodeId(fx.ids()),
                            ExtractorNode::Kind::MinerMk1, nullptr,
                            ExtractorNode::Purity::Normal, gen);
    LogisticsNode storage(ax::NodeEditor::NodeId(fx.ids()),
                          LogisticsNode::Kind::Storage, 1, 1, gen);
    MergerNode origin(ax::NodeEditor::NodeId(fx.ids()), gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(
        fx.LinkPins(extractor.outs[0].get(), storage.ins[0].get()));
    links.push_back(fx.LinkPins(storage.outs[0].get(), origin.ins[0].get()));

    const unsigned long long next_id = fx.ids.n;
    PropagateExtractorResourceUpstream(&origin, &fx.ore, gen);

    REQUIRE(extractor.resource == &fx.ore);
    REQUIRE(extractor.outs[0]->item == &fx.ore);
    REQUIRE(fx.ids.n == next_id);
}

TEST_CASE("PropagateExtractorResourceUpstream preserves choices and handles no-op branches",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    ExtractorNode selected(ax::NodeEditor::NodeId(fx.ids()),
                           ExtractorNode::Kind::MinerMk1, &fx.other,
                           ExtractorNode::Purity::Normal, gen);
    MergerNode origin(ax::NodeEditor::NodeId(fx.ids()), gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(selected.outs[0].get(), origin.ins[0].get()));

    PropagateExtractorResourceUpstream(&origin, &fx.ore, gen);
    REQUIRE(selected.resource == &fx.other);
    REQUIRE(selected.outs[0]->item == &fx.other);

    const unsigned long long before_null_calls = fx.ids.n;
    PropagateExtractorResourceUpstream(nullptr, &fx.ore, gen);
    PropagateExtractorResourceUpstream(&origin, nullptr, gen);
    REQUIRE(fx.ids.n == before_null_calls);

    CraftNode non_graph_origin(ax::NodeEditor::NodeId(fx.ids()), &fx.recipe, gen);
    const unsigned long long next_id = fx.ids.n;
    PropagateExtractorResourceUpstream(&non_graph_origin, &fx.ore, gen);
    REQUIRE(fx.ids.n == next_id);
    REQUIRE(selected.resource == &fx.other);
}

TEST_CASE("PropagateExtractorResourceUpstream creates a missing extractor output deterministically",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    ExtractorNode extractor(ax::NodeEditor::NodeId(fx.ids()),
                            ExtractorNode::Kind::MinerMk1, nullptr,
                            ExtractorNode::Purity::Normal, gen);
    auto detached_output = std::move(extractor.outs[0]);
    extractor.outs.clear();

    MergerNode origin(ax::NodeEditor::NodeId(fx.ids()), gen);
    auto link = fx.LinkPins(detached_output.get(), origin.ins[0].get());
    const unsigned long long expected_pin_id = fx.ids.n;

    PropagateExtractorResourceUpstream(&origin, &fx.ore, gen);

    REQUIRE(extractor.resource == &fx.ore);
    REQUIRE(extractor.outs.size() == 1);
    REQUIRE(extractor.outs[0]->item == &fx.ore);
    REQUIRE(extractor.outs[0]->id.Get() == expected_pin_id);
    REQUIRE(fx.ids.n == expected_pin_id + 1);
}

TEST_CASE("PropagateExtractorResourceUpstream terminates on cycles and ignores craft producers",
          "[graph_item_resolve]")
{
    ResolveFixture fx;
    auto gen = fx.Generator();
    CraftNode craft(ax::NodeEditor::NodeId(fx.ids()), &fx.recipe, gen);
    MergerNode first(ax::NodeEditor::NodeId(fx.ids()), gen);
    LogisticsNode second(ax::NodeEditor::NodeId(fx.ids()),
                         LogisticsNode::Kind::Storage, 2, 1, gen);
    std::vector<std::unique_ptr<Link>> links;
    links.push_back(fx.LinkPins(craft.outs[0].get(), second.ins[0].get()));
    links.push_back(fx.LinkPins(first.outs[0].get(), second.ins[1].get()));
    links.push_back(fx.LinkPins(second.outs[0].get(), first.ins[0].get()));

    const unsigned long long next_id = fx.ids.n;
    PropagateExtractorResourceUpstream(&first, &fx.ore, gen);

    REQUIRE(fx.ids.n == next_id);
    REQUIRE(craft.outs[0]->item == &fx.ingot);
}
