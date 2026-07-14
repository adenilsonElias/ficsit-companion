#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

#include "domain/core/fractional_number.hpp"
#include "domain/gamedata/building.hpp"
#include "domain/gamedata/recipe.hpp"
#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"
#include "domain/snapshot/rate_propagation.hpp"

#include "graph_test_helpers.hpp" // IdGen, MakeLink

namespace
{
    // Builds organizer / craft nodes and wires them, so PropagateRates can be
    // exercised on real Node/Pin/Link objects (no game data needed).
    struct PropFixture
    {
        IdGen idgen;
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<std::unique_ptr<Link>> links;

        // Dummy game data kept alive for craft nodes.
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        std::vector<CountedItem> plate_in{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> plate_out{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe plate_recipe{ plate_in, plate_out, &building, false, 4.0, "Recipe_IronPlate_C" };

        auto NewId() { return ax::NodeEditor::NodeId(idgen()); }

        MergerNode* AddMerger()
        {
            auto n = std::make_unique<MergerNode>(NewId(), [this] { return idgen(); }, nullptr);
            MergerNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        CustomSplitterNode* AddSplitter()
        {
            auto n = std::make_unique<CustomSplitterNode>(NewId(), [this] { return idgen(); }, nullptr);
            CustomSplitterNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        CraftNode* AddCraft(const FractionalNumber& rate)
        {
            auto n = std::make_unique<CraftNode>(NewId(), &plate_recipe, [this] { return idgen(); });
            n->UpdateRate(rate);
            CraftNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        void Connect(Pin* out_pin, Pin* in_pin)
        {
            links.push_back(MakeLink(idgen(), out_pin, in_pin));
        }
        void Run(const std::unordered_set<const Node*>& mixed = {})
        {
            PropagateRates(nodes, links, mixed);
        }
    };
}

/// @test A merger's single output carries the sum of its input pin rates.
TEST_CASE("merger output equals sum of inputs", "[rate_propagation]")
{
    PropFixture fx;
    MergerNode* m = fx.AddMerger();
    m->ins[0]->current_rate = FractionalNumber(10, 1);
    m->ins[1]->current_rate = FractionalNumber(15, 1);
    m->ins[2]->current_rate = FractionalNumber(5, 1);

    fx.Run();

    REQUIRE(m->outs[0]->current_rate == FractionalNumber(30, 1));
}

/// @test A splitter divides its input equally among only the CONNECTED outputs.
TEST_CASE("splitter divides input among connected outputs", "[rate_propagation]")
{
    PropFixture fx;
    CustomSplitterNode* s = fx.AddSplitter();
    MergerNode* sink = fx.AddMerger(); // just to provide input pins to link into
    s->ins[0]->current_rate = FractionalNumber(6, 1);
    // Connect 2 of the splitter's 3 outputs.
    fx.Connect(s->outs[0].get(), sink->ins[0].get());
    fx.Connect(s->outs[1].get(), sink->ins[1].get());

    fx.Run();

    REQUIRE(s->outs[0]->current_rate == FractionalNumber(3, 1));
    REQUIRE(s->outs[1]->current_rate == FractionalNumber(3, 1));
    // Unconnected output stays at zero (not counted in the division).
    REQUIRE(s->outs[2]->current_rate == FractionalNumber(0, 1));
}

/// @test A link copies the upstream output rate onto the downstream input pin.
TEST_CASE("link copies upstream rate to downstream input", "[rate_propagation]")
{
    PropFixture fx;
    MergerNode* up = fx.AddMerger();
    MergerNode* down = fx.AddMerger();
    up->ins[0]->current_rate = FractionalNumber(7, 1);
    up->ins[1]->current_rate = FractionalNumber(0, 1);
    up->ins[2]->current_rate = FractionalNumber(0, 1);
    fx.Connect(up->outs[0].get(), down->ins[0].get());

    fx.Run();

    REQUIRE(down->ins[0]->current_rate == FractionalNumber(7, 1));
}

/// @test Multi-hop chain (merger -> splitter -> merger) converges in one call.
TEST_CASE("multi-hop chain converges", "[rate_propagation]")
{
    PropFixture fx;
    MergerNode* m = fx.AddMerger();
    CustomSplitterNode* s = fx.AddSplitter();
    MergerNode* tail = fx.AddMerger();
    m->ins[0]->current_rate = FractionalNumber(12, 1);
    m->ins[1]->current_rate = FractionalNumber(0, 1);
    m->ins[2]->current_rate = FractionalNumber(0, 1);
    fx.Connect(m->outs[0].get(), s->ins[0].get());
    fx.Connect(s->outs[0].get(), tail->ins[0].get());
    fx.Connect(s->outs[1].get(), tail->ins[1].get());

    fx.Run();

    // merger out = 12, splitter has 2 connected outs => 6 each, tail sums to 12.
    REQUIRE(s->ins[0]->current_rate == FractionalNumber(12, 1));
    REQUIRE(s->outs[0]->current_rate == FractionalNumber(6, 1));
    REQUIRE(tail->outs[0]->current_rate == FractionalNumber(12, 1));
}

/// @test A CraftNode's input pin keeps its recipe-fixed rate; propagation must
///       never overwrite it with the (different) upstream supply rate.
TEST_CASE("craft input pin is not overwritten by propagation", "[rate_propagation]")
{
    PropFixture fx;
    // Craft at rate 1 => recipe input of 30 ore/min on its input pin.
    CraftNode* craft = fx.AddCraft(FractionalNumber(1, 1));
    REQUIRE(craft->ins.size() == 1);
    const FractionalNumber recipe_demand = craft->ins[0]->current_rate;
    REQUIRE(recipe_demand == FractionalNumber(30, 1));

    // Upstream supplies a DIFFERENT rate into the craft's input.
    MergerNode* up = fx.AddMerger();
    up->ins[0]->current_rate = FractionalNumber(5, 1);
    up->ins[1]->current_rate = FractionalNumber(0, 1);
    up->ins[2]->current_rate = FractionalNumber(0, 1);
    fx.Connect(up->outs[0].get(), craft->ins[0].get());

    fx.Run();

    // The craft's input rate is recipe-fixed and untouched.
    REQUIRE(craft->ins[0]->current_rate == recipe_demand);
}
