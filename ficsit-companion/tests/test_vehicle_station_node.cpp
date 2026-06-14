#include <catch2/catch_test_macros.hpp>

#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/link.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/json.hpp"
#include "domain/recipe.hpp" // Item
#include "domain/vehicle_route.hpp"
#include "graph_test_helpers.hpp" // IdGen

#include <imgui_node_editor.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    class FakeResolver : public INodeDataResolver
    {
    public:
        std::unordered_map<std::string, const Item*> items;
        const Recipe* FindRecipe(const std::string&) const override { return nullptr; }
        const Item* FindItem(const std::string& n) const override
        {
            auto it = items.find(n); return it == items.end() ? nullptr : it->second;
        }
    };

    // Make a route (plug<->plug) link between a loader and an unloader and index
    // it on both stations. The owning Link is kept alive by `storage`.
    Link* LinkPlugs(VehicleStationNode* loader, VehicleStationNode* unloader,
                    IdGen& idgen, std::vector<std::unique_ptr<Link>>& storage)
    {
        storage.emplace_back(std::make_unique<Link>(
            ax::NodeEditor::LinkId(idgen()), loader->plug.get(), unloader->plug.get()));
        Link* l = storage.back().get();
        loader->route_links.push_back(l);
        unloader->route_links.push_back(l);
        return l;
    }
}

/// @test A truck VehicleStationNode builds with 3 inputs (2 cargo + 1 fuel),
///       2 cargo outputs, a vehicle plug, and defaults to Load mode whose plug
///       is an Output (the route source).
/// @covers VehicleStationNode construction, default mode, plug direction.
TEST_CASE("VehicleStationNode builds belts + plug in Load mode", "[vehicle_station]")
{
    IdGen idgen;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });

    REQUIRE(s.IsLogistics());
    REQUIRE(s.logistics_kind == LogisticsNode::Kind::TruckStation);
    REQUIRE(s.ins.size() == 3);   // 2 cargo + 1 fuel
    REQUIRE(s.outs.size() == 2);  // 2 cargo
    REQUIRE(s.plug != nullptr);
    REQUIRE(s.mode == VehicleStationNode::Mode::Load);
    REQUIRE(s.plug->direction == ax::NodeEditor::PinKind::Output);
}

/// @test Switching to Unload flips the plug to an Input and clears any route links.
/// @covers VehicleStationNode::SetMode plug recreation.
TEST_CASE("VehicleStationNode SetMode flips plug direction", "[vehicle_station]")
{
    IdGen idgen;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });

    s.SetMode(VehicleStationNode::Mode::Unload, [&idgen] { return idgen(); });
    REQUIRE(s.mode == VehicleStationNode::Mode::Unload);
    REQUIRE(s.plug->direction == ax::NodeEditor::PinKind::Input);
    REQUIRE(s.route_links.empty());
}

/// @test An Unload-mode truck station re-serializes to identical JSON after a
///       Deserialize round-trip, proving `mode` and belt pins survive reload.
/// @covers VehicleStationNode::Serialize + Node::Deserialize routing.
TEST_CASE("VehicleStationNode round-trips with its mode", "[vehicle_station][serialization]")
{
    IdGen idgen;
    FakeResolver resolver;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });
    s.SetMode(VehicleStationNode::Mode::Unload, [&idgen] { return idgen(); });

    const Json::Value original = s.Serialize();
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);

    REQUIRE(rebuilt->Serialize().Dump() == original.Dump());
    REQUIRE(static_cast<VehicleStationNode*>(rebuilt.get())->mode == VehicleStationNode::Mode::Unload);
}

/// @test PropagateCargoItems mirrors a single routed item onto BOTH of an
///       unloader's output belts (a station's belts all carry its one item),
///       and back-fills an empty loader from a typed unloader.
/// @covers VehicleRoute::PropagateCargoItems single-item uniform fill.
TEST_CASE("PropagateCargoItems marks every belt for a single item", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor; // one loader cargo input, the other empty

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    VehicleRoute::PropagateCargoItems(VehicleRoute::FindPool(&A));

    REQUIRE(D.outs[0]->item == &rotor);
    REQUIRE(D.outs[1]->item == &rotor); // both unloader belts marked
    REQUIRE(A.ins[1]->item == &rotor);  // loader's other belt mirrored too
}

/// @test With two distinct items routed, PropagateCargoItems keeps one item per
///       belt instead of overwriting — the uniform fill only applies to a
///       single-item station.
/// @covers VehicleRoute::PropagateCargoItems multi-item (no over-fill).
TEST_CASE("PropagateCargoItems keeps one belt per item for multi-item routes", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);
    Item screw("Screw", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor;
    A.ins[1]->item = &screw;

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    VehicleRoute::PropagateCargoItems(VehicleRoute::FindPool(&A));

    REQUIRE(D.outs[0]->item == &rotor);
    REQUIRE(D.outs[1]->item == &screw); // one belt each, not both the same
}

/// @test FindPool returns every station reachable through route links (A,B,C->D
///       is one pool of four), regardless of which member you start from.
/// @covers VehicleRoute::FindPool connected-component traversal.
TEST_CASE("FindPool gathers the connected route component", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode B(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode C(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);
    LinkPlugs(&B, &D, idgen, storage);
    LinkPlugs(&C, &D, idgen, storage);

    std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(&D);
    REQUIRE(pool.size() == 4);

    std::vector<VehicleStationNode*> from_a = VehicleRoute::FindPool(&A);
    REQUIRE(from_a.size() == 4);
}

/// @test SummarizePool sums loader cargo-input rates as supply and unloader
///       cargo-output rates as demand, per item, across the pool.
/// @covers VehicleRoute::SummarizePool.
TEST_CASE("SummarizePool reports per-item supply vs demand", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);

    // Loader A: first cargo input carries 60 Rotor/min.
    A.ins[0]->item = &rotor;
    A.ins[0]->current_rate = FractionalNumber(60, 1);
    // Unloader D: first cargo output carries 60 Rotor/min.
    D.outs[0]->item = &rotor;
    D.outs[0]->current_rate = FractionalNumber(60, 1);

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto balances = VehicleRoute::SummarizePool(VehicleRoute::FindPool(&A));
    REQUIRE(balances.size() == 1);
    REQUIRE(balances[0].item == &rotor);
    REQUIRE(balances[0].supply == FractionalNumber(60, 1));
    REQUIRE(balances[0].demand == FractionalNumber(60, 1));
}

/// @test GroupPoolByItem puts a loader's cargo-input pin in `supply` and an
///       unloader's cargo-output pin in `demand` for the shared item.
/// @covers VehicleRoute::GroupPoolByItem supply/demand partition.
TEST_CASE("GroupPoolByItem partitions supply and demand", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor;
    D.outs[0]->item = &rotor;

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto groups = VehicleRoute::GroupPoolByItem(VehicleRoute::FindPool(&A));
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].item == &rotor);
    REQUIRE(groups[0].supply.size() == 1);
    REQUIRE(groups[0].supply[0] == A.ins[0].get());
    REQUIRE(groups[0].demand.size() == 1);
    REQUIRE(groups[0].demand[0] == D.outs[0].get());
}

/// @test GroupPoolByItem never treats the fuel inlet (last input of a loader)
///       as cargo supply, even when it carries an item.
/// @covers VehicleRoute::GroupPoolByItem fuel-pin exclusion.
TEST_CASE("GroupPoolByItem excludes the fuel inlet", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);
    Item fuel("Fuel", "", 0);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor;
    A.ins.back()->item = &fuel; // dedicated fuel inlet
    D.outs[0]->item = &rotor;

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto groups = VehicleRoute::GroupPoolByItem(VehicleRoute::FindPool(&A));
    REQUIRE(groups.size() == 1); // only Rotor, never Fuel
    REQUIRE(groups[0].item == &rotor);
    REQUIRE(groups[0].supply.size() == 1);
    REQUIRE(groups[0].supply[0] == A.ins[0].get());
}

/// @test GroupPoolByItem skips cargo pins with no item assigned yet.
/// @covers VehicleRoute::GroupPoolByItem null-item skip.
TEST_CASE("GroupPoolByItem skips null-item pins", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor; // ins[1] left null
    D.outs[0]->item = &rotor; // outs[1] left null

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto groups = VehicleRoute::GroupPoolByItem(VehicleRoute::FindPool(&A));
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].supply.size() == 1); // null ins[1] excluded
    REQUIRE(groups[0].demand.size() == 1); // null outs[1] excluded
}

/// @test GroupPoolByItem omits an item present on only one side (permissive):
///       no group is emitted so the item is never forced to zero.
/// @covers VehicleRoute::GroupPoolByItem permissive filtering.
TEST_CASE("GroupPoolByItem omits one-sided items", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);
    Item screw("Screw", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor;   // supply only
    D.outs[0]->item = &screw;  // demand only

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto groups = VehicleRoute::GroupPoolByItem(VehicleRoute::FindPool(&A));
    REQUIRE(groups.empty());
}

/// @test GroupPoolByItem produces one independent group per item in a
///       multi-item pool.
/// @covers VehicleRoute::GroupPoolByItem multi-item grouping.
TEST_CASE("GroupPoolByItem groups each item independently", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);
    Item screw("Screw", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);
    A.ins[0]->item = &rotor;
    A.ins[1]->item = &screw;
    D.outs[0]->item = &rotor;
    D.outs[1]->item = &screw;

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto groups = VehicleRoute::GroupPoolByItem(VehicleRoute::FindPool(&A));
    REQUIRE(groups.size() == 2);
    auto find = [&](const Item* it) -> const VehicleRoute::PoolItemGroup* {
        for (const auto& g : groups) if (g.item == it) return &g;
        return nullptr;
    };
    REQUIRE(find(&rotor) != nullptr);
    REQUIRE(find(&screw) != nullptr);
    REQUIRE(find(&rotor)->supply.size() == 1);
    REQUIRE(find(&rotor)->demand.size() == 1);
    REQUIRE(find(&screw)->supply.size() == 1);
    REQUIRE(find(&screw)->demand.size() == 1);
}
