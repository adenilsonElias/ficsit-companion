#include <catch2/catch_test_macros.hpp>

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"
#include "domain/gamedata/recipe.hpp"
#include "domain/gamedata/building.hpp"

#include "graph_test_helpers.hpp" // IdGen

#include <memory>
#include <vector>

namespace
{
    // A group wrapping a single 30 ore -> 20 plate craft (at rate 1), no internal
    // links. Holds the fixture data so the recipe/item/building outlive the group.
    struct GroupFixture
    {
        IdGen idgen;
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        std::vector<CountedItem> ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe recipe{ ins, outs, &building, false, 4.0, "Recipe_IronPlate_C" };

        CraftNode* craft = nullptr;

        std::unique_ptr<GroupNode> MakeGroup()
        {
            std::vector<std::unique_ptr<Node>> subnodes;
            subnodes.push_back(std::make_unique<CraftNode>(
                ax::NodeEditor::NodeId(idgen()), &recipe, [this] { return idgen(); }));
            craft = static_cast<CraftNode*>(subnodes[0].get());
            std::vector<std::unique_ptr<Link>> sublinks;
            return std::make_unique<GroupNode>(ax::NodeEditor::NodeId(idgen()),
                [this] { return idgen(); }, std::move(subnodes), std::move(sublinks));
        }
    };
}

/// @test   A group wrapping a single 30-ore→20-plate craft exposes the craft's net flows as external
///         pins: one input pin at 30/min (consumed ore), one output pin at 20/min (produced plate),
///         and an aggregated machine count of one constructor.
/// @covers GroupNode construction + UpdateDetails — collapsing a subgraph's unmatched consumed/
///         produced items into the group's external pins and rolling up total_machines.
TEST_CASE("group exposes the craft's net input/output as external pins", "[group][node]")
{
    GroupFixture fx;
    std::unique_ptr<GroupNode> group = fx.MakeGroup();

    // One consumed item (ore) -> one input pin; one produced item (plate) -> one output pin.
    REQUIRE(group->ins.size() == 1);
    REQUIRE(group->outs.size() == 1);
    REQUIRE(group->ins[0]->current_rate == FractionalNumber(30, 1));
    REQUIRE(group->outs[0]->current_rate == FractionalNumber(20, 1));
    // UpdateDetails aggregated one machine of the craft's building.
    REQUIRE(group->total_machines.at("Test_Constructor") == FractionalNumber(1, 1));
}

/// @test   Calling UpdateRate(2) on the group scales the whole subgraph by ×2: the inner craft goes
///         to 2 machines (60 ore in / 40 plate out), the group's external pins follow (60 / 40),
///         total_machines becomes 2, and power is recomputed to a non-zero fixed value.
/// @covers GroupNode::UpdateRate fan-out — proportional scaling of subnodes, external pins, machine
///         aggregates, and same_clock_power, plus HasVariablePower() reporting false for this group.
TEST_CASE("group UpdateRate scales subnodes, external pins, machines and power", "[group][node]")
{
    GroupFixture fx;
    std::unique_ptr<GroupNode> group = fx.MakeGroup();

    group->UpdateRate(FractionalNumber(2, 1)); // double the whole group

    // Subnode craft scaled to 2 machines: 60 ore in, 40 plate out.
    REQUIRE(fx.craft->current_rate == FractionalNumber(2, 1));
    REQUIRE(fx.craft->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(fx.craft->outs[0]->current_rate == FractionalNumber(40, 1));

    // Group's external pins scale proportionally.
    REQUIRE(group->ins[0]->current_rate == FractionalNumber(60, 1));
    REQUIRE(group->outs[0]->current_rate == FractionalNumber(40, 1));

    // Aggregates refreshed.
    REQUIRE(group->total_machines.at("Test_Constructor") == FractionalNumber(2, 1));
    // Power was computed from the building (4 MW, exponent 1.6) -> non-zero.
    REQUIRE(group->same_clock_power.GetNumerator() != 0);
    REQUIRE(group->HasVariablePower() == false);
}

/// @test   SetBuiltState(true) propagates into the group: the inner craft is flagged built and the
///         group's built_machines count rises from zero to equal its total_machines.
/// @covers GroupNode::SetBuiltState — pushing the built flag down to craft subnodes and refreshing
///         the built-machine aggregate used by the "already built" accounting.
TEST_CASE("group SetBuiltState marks craft subnodes and updates built machines", "[group][node]")
{
    GroupFixture fx;
    std::unique_ptr<GroupNode> group = fx.MakeGroup();

    REQUIRE(fx.craft->built == false);
    REQUIRE(group->built_machines.at("Test_Constructor") == FractionalNumber(0, 1));

    group->SetBuiltState(true);

    REQUIRE(fx.craft->built == true);
    // Now every machine counts as built.
    REQUIRE(group->built_machines.at("Test_Constructor") == group->total_machines.at("Test_Constructor"));
}

// ---------------------------------------------------------------------------
// A group whose internal subgraph contains a fan-out: one producer feeding two
// consumers off the same output pin, with no splitter node between them.
// ---------------------------------------------------------------------------

namespace
{
    // A 30 ore -> 20 plate producer feeding TWO 20 plate -> 10 rod consumers from one output pin.
    // The producer runs at 3 machines (60 plate out), split 40 / 20 across the two branches, so
    // the group internally balances plate and only ore in / rod out cross its boundary.
    struct FanOutGroupFixture
    {
        IdGen idgen;
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        Item rod{ "Iron Rod", "", 3 };

        std::vector<CountedItem> prod_ins{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> prod_outs{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe producer_recipe{ prod_ins, prod_outs, &building, false, 4.0, "Recipe_IronPlate_C" };

        std::vector<CountedItem> cons_ins{ CountedItem(&plate, FractionalNumber(20, 1)) };
        std::vector<CountedItem> cons_outs{ CountedItem(&rod, FractionalNumber(10, 1)) };
        Recipe consumer_recipe{ cons_ins, cons_outs, &building, false, 4.0, "Recipe_IronRod_C" };

        CraftNode* producer = nullptr;
        CraftNode* consumer_a = nullptr;
        CraftNode* consumer_b = nullptr;

        std::unique_ptr<GroupNode> MakeGroup()
        {
            auto gen = [this] { return idgen(); };
            std::vector<std::unique_ptr<Node>> subnodes;
            subnodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(idgen()), &producer_recipe, gen));
            subnodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(idgen()), &consumer_recipe, gen));
            subnodes.push_back(std::make_unique<CraftNode>(ax::NodeEditor::NodeId(idgen()), &consumer_recipe, gen));
            producer = static_cast<CraftNode*>(subnodes[0].get());
            consumer_a = static_cast<CraftNode*>(subnodes[1].get());
            consumer_b = static_cast<CraftNode*>(subnodes[2].get());

            producer->UpdateRate(FractionalNumber(3, 1));   // 60 plate out
            consumer_a->UpdateRate(FractionalNumber(2, 1)); // 40 plate in
            consumer_b->UpdateRate(FractionalNumber(1, 1)); // 20 plate in

            std::vector<std::unique_ptr<Link>> sublinks;
            sublinks.push_back(MakeLink(idgen(), producer->outs[0].get(), consumer_a->ins[0].get()));
            sublinks.back()->current_rate = FractionalNumber(40, 1);
            sublinks.push_back(MakeLink(idgen(), producer->outs[0].get(), consumer_b->ins[0].get()));
            sublinks.back()->current_rate = FractionalNumber(20, 1);

            return std::make_unique<GroupNode>(ax::NodeEditor::NodeId(idgen()), gen,
                std::move(subnodes), std::move(sublinks));
        }
    };
}

/// @test   A group holding an internal fan-out keeps both branches and exposes only what actually
///         crosses its boundary: ore in and rod out. The plate is fully consumed inside, so it
///         produces no external pin even though it flows over two separate belts in there.
/// @covers GroupNode construction over a subgraph with a multi-link pin. CreateInsOuts balances by
///         item (produced vs consumed), not by counting links, so a fan-out inside the group must
///         net out exactly like a single belt carrying the same total - and both edges must survive
///         being moved into the group.
TEST_CASE("group wraps an internal fan-out without losing a branch", "[group][node][multilink]")
{
    FanOutGroupFixture fx;
    std::unique_ptr<GroupNode> group = fx.MakeGroup();

    // Both branches came along, hanging off the one shared output pin.
    REQUIRE(group->links.size() == 2);
    REQUIRE(fx.producer->outs[0]->links.size() == 2);
    REQUIRE(group->links[0]->current_rate == FractionalNumber(40, 1));
    REQUIRE(group->links[1]->current_rate == FractionalNumber(20, 1));

    // Plate is produced (60) and consumed (40 + 20) inside, so it nets to zero and stays internal:
    // only ore crosses in, only rod crosses out.
    REQUIRE(group->ins.size() == 1);
    REQUIRE(group->ins[0]->item == &fx.ore);
    REQUIRE(group->ins[0]->current_rate == FractionalNumber(90, 1)); // 3 machines x 30 ore
    REQUIRE(group->outs.size() == 1);
    REQUIRE(group->outs[0]->item == &fx.rod);
    REQUIRE(group->outs[0]->current_rate == FractionalNumber(30, 1)); // (2 + 1) machines x 10 rod
}
