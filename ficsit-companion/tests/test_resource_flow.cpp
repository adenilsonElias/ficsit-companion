#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "domain/building.hpp"
#include "domain/fractional_number.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"
#include "domain/resource_flow.hpp"

#include "graph_test_helpers.hpp" // IdGen

namespace
{
    // 30 ore -> 20 plate, and 20 plate -> 10 rod, in a dummy building (no game
    // data needed). Holds fixture data so items/recipes outlive the nodes.
    struct FlowFixture
    {
        IdGen idgen;
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        Item rod{ "Iron Rod", "", 3 };

        std::vector<CountedItem> plate_in{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> plate_out{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe plate_recipe{ plate_in, plate_out, &building, false, 4.0, "Recipe_IronPlate_C" };

        std::vector<CountedItem> rod_in{ CountedItem(&plate, FractionalNumber(20, 1)) };
        std::vector<CountedItem> rod_out{ CountedItem(&rod, FractionalNumber(10, 1)) };
        Recipe rod_recipe{ rod_in, rod_out, &building, false, 4.0, "Recipe_IronRod_C" };

        std::unique_ptr<CraftNode> MakeCraft(const Recipe& r, const FractionalNumber& rate)
        {
            auto craft = std::make_unique<CraftNode>(
                ax::NodeEditor::NodeId(idgen()), &r, [this] { return idgen(); });
            craft->UpdateRate(rate);
            return craft;
        }

        const ResourceFlowRow* Find(const ResourceFlowReport& rep, const Item* item)
        {
            for (const auto& row : rep.rows)
            {
                if (row.item == item) return &row;
            }
            return nullptr;
        }
    };
}

/// @test   A single craft at rate 1 reports its recipe inputs as consumed and
///         outputs as produced: ore 30 consumed (deficit), plate 20 produced
///         (surplus); rows are name-sorted, counts tally, and nothing is skipped.
/// @covers BuildResourceFlowReport craft branch, status classification, counts,
///         and ItemPtrCompare-based row ordering.
TEST_CASE("single craft reports consumed inputs and produced outputs", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.size() == 2);
    REQUIRE(rep.skipped_null_item_pins == 0);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(ore != nullptr);
    REQUIRE(plate != nullptr);

    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(ore->produced == FractionalNumber(0, 1));
    REQUIRE(ore->net == FractionalNumber(-30, 1));
    REQUIRE(ore->status == ResourceFlowStatus::Deficit);

    REQUIRE(plate->produced == FractionalNumber(20, 1));
    REQUIRE(plate->consumed == FractionalNumber(0, 1));
    REQUIRE(plate->net == FractionalNumber(20, 1));
    REQUIRE(plate->status == ResourceFlowStatus::Surplus);

    REQUIRE(rep.rows[0].item == &fx.ore);
    REQUIRE(rep.rows[1].item == &fx.plate);

    REQUIRE(rep.deficit_count == 1);
    REQUIRE(rep.surplus_count == 1);
    REQUIRE(rep.balanced_count == 0);
}

/// @test   A craft chain reports gross per-item flow: an internal intermediate
///         produced and consumed in equal amounts is Balanced, while the raw
///         input is a deficit and the final output a surplus. Exercises all
///         three statuses and their counters in one graph.
/// @covers BuildResourceFlowReport gross accumulation across multiple craft
///         nodes and the Balanced classification path.
TEST_CASE("craft chain marks a fully-internal intermediate as balanced", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1))); // +20 plate, -30 ore
    nodes.push_back(fx.MakeCraft(fx.rod_recipe, FractionalNumber(1, 1)));   // -20 plate, +10 rod

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    const ResourceFlowRow* rod = fx.Find(rep, &fx.rod);
    REQUIRE(ore != nullptr);
    REQUIRE(plate != nullptr);
    REQUIRE(rod != nullptr);

    // Intermediate fully internal => balanced.
    REQUIRE(plate->produced == FractionalNumber(20, 1));
    REQUIRE(plate->consumed == FractionalNumber(20, 1));
    REQUIRE(plate->net == FractionalNumber(0, 1));
    REQUIRE(plate->status == ResourceFlowStatus::Balanced);

    REQUIRE(ore->status == ResourceFlowStatus::Deficit);
    REQUIRE(rod->status == ResourceFlowStatus::Surplus);

    REQUIRE(rep.deficit_count == 1);
    REQUIRE(rep.balanced_count == 1);
    REQUIRE(rep.surplus_count == 1);
}

/// @test   An extractor's output is counted as production and offsets a craft's
///         raw-resource deficit: a miner producing 30 ore against a craft
///         consuming 30 ore nets the ore to 0 (balanced).
/// @covers BuildResourceFlowReport extractor branch (outs counted as produced).
TEST_CASE("extractor output offsets a craft's raw-resource deficit", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1))); // consumes 30 ore

    auto extractor = std::make_unique<ExtractorNode>(
        ax::NodeEditor::NodeId(fx.idgen()), ExtractorNode::Kind::MinerMk2, &fx.ore,
        ExtractorNode::Purity::Normal, [&fx] { return fx.idgen(); });
    extractor->outs[0]->current_rate = FractionalNumber(30, 1); // set directly, skip power path
    nodes.push_back(std::move(extractor));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    REQUIRE(ore != nullptr);
    REQUIRE(ore->produced == FractionalNumber(30, 1));
    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(ore->net == FractionalNumber(0, 1));
    REQUIRE(ore->status == ResourceFlowStatus::Balanced);
}

/// @test   A sink's input is counted as consumption: a sink consuming 20 plate
///         (with nothing producing it) reports plate as a 20/min deficit.
/// @covers BuildResourceFlowReport sink branch (ins counted as consumed).
TEST_CASE("sink input is counted as consumption", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, &fx.plate);
    sink->ins[0]->current_rate = FractionalNumber(20, 1);
    nodes.push_back(std::move(sink));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(plate != nullptr);
    REQUIRE(plate->consumed == FractionalNumber(20, 1));
    REQUIRE(plate->net == FractionalNumber(-20, 1));
    REQUIRE(plate->status == ResourceFlowStatus::Deficit);
}

/// @test   Inspected pins with a null item are not tallied but are counted in
///         skipped_null_item_pins (here: a sink created with no item).
/// @covers BuildResourceFlowReport null-item handling in AccumulatePins.
TEST_CASE("null-item pins are skipped and counted", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, nullptr);
    sink->ins[0]->current_rate = FractionalNumber(5, 1);
    nodes.push_back(std::move(sink));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.empty());
    REQUIRE(rep.skipped_null_item_pins == 1);
}

/// @test   Group contents are analyzed recursively (gross): a group wrapping a
///         rate-1 craft contributes the craft's 30 ore consumed and 20 plate
///         produced to the report, exactly as if the craft were top-level.
/// @covers BuildResourceFlowReport group branch (recurse into group->nodes).
TEST_CASE("group contents are analyzed recursively", "[resource_flow]")
{
    FlowFixture fx;

    std::vector<std::unique_ptr<Node>> subnodes;
    subnodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));
    std::vector<std::unique_ptr<Link>> sublinks;

    auto group = std::make_unique<GroupNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); },
        std::move(subnodes), std::move(sublinks));
    group->UpdateRate(FractionalNumber(1, 1));

    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::move(group));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(ore != nullptr);
    REQUIRE(plate != nullptr);
    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(plate->produced == FractionalNumber(20, 1));
}

/// @test   Organizer and logistics nodes create no production or consumption: a
///         graph of only a merger and a truck station yields an empty report
///         with nothing skipped (their pins are never inspected).
/// @covers BuildResourceFlowReport exclusion of IsOrganizer()/IsLogistics().
TEST_CASE("organizers and logistics contribute nothing", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    nodes.push_back(std::make_unique<MergerNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, &fx.plate));
    nodes.push_back(std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(fx.idgen()), LogisticsNode::Kind::TruckStation, 1, 1,
        [&fx] { return fx.idgen(); }));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.empty());
    REQUIRE(rep.skipped_null_item_pins == 0);
}

/// @test   RowPassesFilter gates by status and case-insensitive name search:
///         All passes every status; Deficit/Surplus pass only matching rows;
///         empty search matches all; name search is case-insensitive.
/// @covers RowPassesFilter status gate + name search.
TEST_CASE("RowPassesFilter gates by status and name", "[resource_flow]")
{
    FlowFixture fx;
    ResourceFlowRow surplus;
    surplus.item = &fx.plate; // "Iron Plate"
    surplus.status = ResourceFlowStatus::Surplus;

    ResourceFlowRow deficit;
    deficit.item = &fx.ore;   // "Iron Ore"
    deficit.status = ResourceFlowStatus::Deficit;

    // Status filter
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, ""));
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::Surplus, ""));
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::Deficit, ""));
    REQUIRE(RowPassesFilter(deficit, ResourceFlowFilter::Deficit, ""));
    REQUIRE_FALSE(RowPassesFilter(deficit, ResourceFlowFilter::Surplus, ""));

    // Name search (case-insensitive), combined with All
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, "plate"));
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, "PLATE"));
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::All, "ore"));
    // Search still respects the status gate
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::Deficit, "plate"));
}

/// @test   Fractional rates are preserved exactly: a craft producing 20 plate
///         against another consuming 20/3 plate nets plate to exactly 40/3.
/// @covers BuildResourceFlowReport exact-rational accumulation (no float drift).
TEST_CASE("fractional rates preserve exact net values", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));        // +20 plate
    nodes.push_back(fx.MakeCraft(fx.rod_recipe, FractionalNumber(1, 3)));          // consumes 20*(1/3)=20/3 plate

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(plate != nullptr);
    REQUIRE(plate->produced == FractionalNumber(20, 1));
    REQUIRE(plate->consumed == FractionalNumber(20, 3));
    REQUIRE(plate->net == FractionalNumber(40, 3));
    REQUIRE(plate->status == ResourceFlowStatus::Surplus);
}
