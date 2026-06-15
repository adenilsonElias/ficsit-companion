#include <catch2/catch_test_macros.hpp>

#include <imgui_node_editor.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "domain/game_data.hpp"
#include "domain/node.hpp"
#include "domain/recipe.hpp"
#include "infra/sav_import.hpp"

#include "graph_test_helpers.hpp"

namespace
{
    void EnsureGameDataLoaded()
    {
        static bool loaded = false;
        if (!loaded)
        {
            const char* candidates[] = {
                "satisfactory",
                "assets/satisfactory",
                "../assets/satisfactory",
            };
            for (const char* candidate : candidates)
            {
                if (std::filesystem::exists(std::string(candidate) + ".json"))
                {
                    Data::LoadData(candidate);
                    loaded = true;
                    return;
                }
            }
            const std::filesystem::path source_path(__FILE__);
            const std::filesystem::path repo_root =
                source_path.parent_path().parent_path().parent_path();
            const std::filesystem::path asset_base = repo_root / "assets" / "satisfactory";
            Data::LoadData(asset_base.string());
            loaded = true;
        }
    }

    SavImport::Building Manufacturer(
        const std::string& id,
        const std::string& recipe,
        const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Manufacturer;
        b.recipe_name = recipe;
        b.clock = 1.0;
        b.x = x;
        return b;
    }

    SavImport::Building Merger(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Merger;
        b.x = x;
        return b;
    }

    SavImport::Building Splitter(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Splitter;
        b.x = x;
        return b;
    }

    SavImport::Building SmartSplitter(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::SmartSplitter;
        b.x = x;
        return b;
    }

    SavImport::Building Miner(
        const std::string& id,
        const std::string& resource,
        const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Miner;
        b.item_name = resource;
        b.extractor_kind = 1;
        b.clock = 1.0;
        b.x = x;
        return b;
    }

    SavImport::Building TruckStation(
        const std::string& id,
        const float x,
        const std::string& fuel_item,
        const std::string& cargo_item,
        const bool is_unloader)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::TruckStation;
        b.fuel_item = fuel_item;
        b.cargo_item = cargo_item;
        b.is_unloader = is_unloader;
        b.x = x;
        return b;
    }

    SavImport::Belt Belt(
        const std::string& id,
        const std::string& src,
        const int src_port,
        const std::string& dst,
        const int dst_port)
    {
        SavImport::Belt belt;
        belt.id = id;
        belt.src_building = src;
        belt.src_port = src_port;
        belt.src_dir = "out";
        belt.dst_building = dst;
        belt.dst_port = dst_port;
        belt.dst_dir = "in";
        return belt;
    }

    SavImport::Building Sink(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Sink;
        b.x = x;
        return b;
    }

    bool HasWarningContaining(
        const std::vector<std::string>& warnings,
        const std::string& needle)
    {
        return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& warning) {
            return warning.find(needle) != std::string::npos;
        });
    }
}

/// @test   A save import whose plain merger is fed by two different concrete
///         item streams is reported as a mixed-item pass-through instead of
///         being silently typed as whichever item the propagation pass sees
///         first.
/// @covers SavImport::BuildGraph item inference through organizers and import
///         warnings for invalid mixed-item merger chains.
TEST_CASE("BuildGraph warns when a merger receives different item streams", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("plastic_refinery", "Plastic", 0.0f));
    parsed.buildings.push_back(Manufacturer("sheet_constructor", "Copper Sheet", 100.0f));
    parsed.buildings.push_back(Merger("mixed_merger", 200.0f));
    parsed.belts.push_back(Belt("plastic_to_merger", "plastic_refinery", 0, "mixed_merger", 0));
    parsed.belts.push_back(Belt("sheet_to_merger", "sheet_constructor", 0, "mixed_merger", 1));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;

    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(HasWarningContaining(out.warnings, "mixed item"));
    REQUIRE(HasWarningContaining(out.warnings, "mixed_merger"));
    REQUIRE(HasWarningContaining(out.warnings, "Plastic"));
    REQUIRE(HasWarningContaining(out.warnings, "Copper Sheet"));

    REQUIRE(out.nodes.size() == 3);
    REQUIRE(out.nodes[2]->IsMerger());
    REQUIRE(static_cast<MergerNode*>(out.nodes[2].get())->item == nullptr);
}

/// @test   Mixed-item detection does not use downstream multi-input craft pins
///         as proof that a splitter itself carries multiple items. Imported
///         saves can initially route unresolved belts to multi-input recipes by
///         physical save-port rank; those consumer pins are demand hints, not
///         producer evidence.
/// @covers SavImport::BuildGraph mixed-item warning evidence selection.
TEST_CASE("BuildGraph does not report splitter mixed items from downstream craft demand only", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("plastic_refinery", "Plastic", 0.0f));
    parsed.buildings.push_back(Manufacturer("sheet_constructor", "Copper Sheet", 100.0f));
    parsed.buildings.push_back(Splitter("plastic_splitter", 200.0f));
    parsed.buildings.push_back(Splitter("sheet_splitter", 300.0f));
    parsed.buildings.push_back(Manufacturer("circuit_board_a", "Circuit Board", 400.0f));
    parsed.buildings.push_back(Manufacturer("circuit_board_b", "Circuit Board", 500.0f));

    parsed.belts.push_back(Belt("plastic_to_splitter", "plastic_refinery", 0, "plastic_splitter", 0));
    parsed.belts.push_back(Belt("sheet_to_splitter", "sheet_constructor", 0, "sheet_splitter", 0));

    parsed.belts.push_back(Belt("plastic_to_a", "plastic_splitter", 0, "circuit_board_a", -1));
    parsed.belts.push_back(Belt("plastic_to_b", "plastic_splitter", 1, "circuit_board_b", 0));
    parsed.belts.push_back(Belt("sheet_to_a", "sheet_splitter", 0, "circuit_board_a", 0));
    parsed.belts.push_back(Belt("sheet_to_b", "sheet_splitter", 1, "circuit_board_b", -1));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;

    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(!HasWarningContaining(out.warnings, "mixed item pass-through at plastic_splitter"));
    REQUIRE(!HasWarningContaining(out.warnings, "mixed item pass-through at sheet_splitter"));
}

/// @test   A known resource flowing through a smart splitter into a plain
///         splitter remains authoritative when the downstream machine has
///         multiple recipe inputs. The import must not use the first physical
///         input slot as a fallback and repaint the upstream splitter as that
///         ingredient.
/// @covers SavImport::BuildGraph item-aware routing through smart splitter
///         outputs toward multi-input craft endpoints.
TEST_CASE("BuildGraph keeps upstream resource item through smart splitter before multi-input craft", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("coal_miner", "Coal", 0.0f));
    parsed.buildings.push_back(SmartSplitter("smart_splitter", 100.0f));
    parsed.buildings.push_back(Splitter("coal_splitter", 200.0f));
    parsed.buildings.push_back(Manufacturer("steel_foundry", "Steel Ingot", 300.0f));

    parsed.belts.push_back(Belt("miner_to_smart", "coal_miner", 0, "smart_splitter", 0));
    parsed.belts.push_back(Belt("smart_to_splitter", "smart_splitter", 0, "coal_splitter", 0));
    parsed.belts.push_back(Belt("splitter_to_foundry", "coal_splitter", 0, "steel_foundry", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;

    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 4);
    REQUIRE(out.nodes[2]->IsGameSplitter());
    const auto* splitter = static_cast<const GameSplitterNode*>(out.nodes[2].get());
    REQUIRE(splitter->item != nullptr);
    REQUIRE(splitter->item->name == "Coal");

    REQUIRE(out.nodes[3]->IsCraft());
    const auto* foundry = static_cast<const CraftNode*>(out.nodes[3].get());
    auto coal_pin = std::find_if(foundry->ins.begin(), foundry->ins.end(), [](const std::unique_ptr<Pin>& pin) {
        return pin->item != nullptr && pin->item->name == "Coal";
    });
    REQUIRE(coal_pin != foundry->ins.end());
    REQUIRE((*coal_pin)->link != nullptr);
    REQUIRE((*coal_pin)->link->start->node == splitter);
}

/// @test   A miner whose resource the save did not record must resolve to the
///         raw resource it actually mines, not to a smart splitter's filter
///         label. A smart/programmable splitter directly downstream carries its
///         configured filter item (e.g. "Plastic") on its pins even when a
///         different resource physically flows through it; an extractor can only
///         produce a mineable resource, so the importer must reject that filter
///         label and keep walking to the real raw resource (Coal here).
/// @covers SavImport extractor resource resolution: rejecting non-raw
///         downstream labels and traversing smart splitters.
TEST_CASE("BuildGraph resolves a miner past a smart splitter's filter label to the raw resource", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    // Miner with no recorded resource -> resource must be inferred downstream.
    SavImport::Building miner;
    miner.id = "mystery_miner";
    miner.kind = SavImport::BuildingKind::Miner;
    miner.extractor_kind = 1;
    miner.clock = 1.0;
    miner.x = 0.0f;
    parsed.buildings.push_back(miner);
    // Smart splitter the player configured with a "Plastic" output filter, even
    // though Coal physically flows through it.
    SavImport::Building smart;
    smart.id = "filter_splitter";
    smart.kind = SavImport::BuildingKind::SmartSplitter;
    smart.item_name = "Plastic";
    smart.x = 100.0f;
    parsed.buildings.push_back(smart);
    // Black Powder = Coal + Sulfur; its first input pin is Coal.
    parsed.buildings.push_back(Manufacturer("black_powder", "Black Powder", 200.0f));

    parsed.belts.push_back(Belt("miner_to_smart", "mystery_miner", 0, "filter_splitter", 0));
    parsed.belts.push_back(Belt("smart_to_craft", "filter_splitter", 0, "black_powder", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 3);
    REQUIRE(out.nodes[0]->IsExtractor());
    const auto* ext = static_cast<const ExtractorNode*>(out.nodes[0].get());
    REQUIRE(ext->resource != nullptr);
    REQUIRE(ext->resource->name == "Coal");
}

/// @test   A belt carrying the station's fuel item (per its FuelInventory) into
///         the station is routed to the dedicated fuel pin, not a cargo pin —
///         even after passing through a splitter. This keeps a vehicle-fuel
///         distribution line (Coal) out of the station's cargo item-space.
/// @covers SavImport::BuildGraph station fuel-belt classification from the
///         exported FuelInventory item.
TEST_CASE("BuildGraph routes a station fuel belt to the fuel pin", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("coal_miner", "Coal", 0.0f));
    parsed.buildings.push_back(Splitter("fuel_splitter", 100.0f));
    parsed.buildings.push_back(TruckStation("station", 200.0f, "Coal", "Iron Ore", true));
    parsed.belts.push_back(Belt("miner_to_split", "coal_miner", 0, "fuel_splitter", 0));
    parsed.belts.push_back(Belt("split_to_station", "fuel_splitter", 0, "station", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 3);
    REQUIRE(out.nodes[2]->IsLogistics());
    Node* station = out.nodes[2].get();
    REQUIRE(!station->ins.empty());
    // The dedicated fuel pin is the last input pin; the Coal belt must land there.
    Pin* fuel_pin = station->ins.back().get();
    REQUIRE(fuel_pin->link != nullptr);
    // No cargo (non-fuel) input pin should carry the fuel belt.
    for (size_t i = 0; i + 1 < station->ins.size(); ++i)
    {
        REQUIRE(station->ins[i]->link == nullptr);
    }
}

/// @test   A loading station fed two belts — Coal (its FuelInventory item) and
///         Iron Ore (its cargo item) — splits them onto the right pins by
///         matching each belt's resolved item to the station's fuel vs cargo
///         inventory, which connector names and item type alone cannot do.
/// @covers SavImport::BuildGraph fuel/cargo belt disambiguation by inventory.
TEST_CASE("BuildGraph splits a loader station's fuel and cargo belts by inventory item", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("coal_miner", "Coal", 0.0f));        // fuel
    parsed.buildings.push_back(Miner("iron_miner", "Iron Ore", 100.0f));  // cargo
    parsed.buildings.push_back(TruckStation("station", 200.0f, "Coal", "Iron Ore", false)); // loader
    parsed.belts.push_back(Belt("coal_to_station", "coal_miner", 0, "station", 0));
    parsed.belts.push_back(Belt("iron_to_station", "iron_miner", 0, "station", 1));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    Node* station = out.nodes[2].get();
    REQUIRE(station->IsLogistics());
    // Fuel pin (last input) carries Coal from the coal miner.
    Pin* fuel_pin = station->ins.back().get();
    REQUIRE(fuel_pin->link != nullptr);
    REQUIRE(fuel_pin->link->start->node == out.nodes[0].get());
    // A cargo input pin carries Iron Ore from the iron miner.
    bool iron_on_cargo = false;
    for (size_t i = 0; i + 1 < station->ins.size(); ++i)
    {
        if (station->ins[i]->link != nullptr &&
            station->ins[i]->link->start->node == out.nodes[1].get())
        {
            iron_on_cargo = true;
        }
    }
    REQUIRE(iron_on_cargo);
}

/// @test   An organizer whose item is already resolved from its upstream producer
///         must not be repainted by a downstream craft's mismatched input pin.
///         A Copper-Ore merger feeding an AI Limiter (Copper Sheet + Quickwire,
///         no Copper Ore input) lands on the Copper Sheet pin by rank; the
///         upstream-ingredient stamp must NOT override the merger to Copper Sheet
///         — upstream supply wins over downstream demand for item identity.
/// @covers SavImport::BuildGraph upstream craft-input stamping not overriding a
///         producer-resolved organizer item.
TEST_CASE("BuildGraph keeps a producer-resolved organizer item against downstream demand", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("copper_miner", "Copper Ore", 0.0f));
    parsed.buildings.push_back(Merger("ore_merger", 100.0f));
    parsed.buildings.push_back(Manufacturer("ai_limiter", "AI Limiter", 200.0f));
    parsed.belts.push_back(Belt("miner_to_merger", "copper_miner", 0, "ore_merger", 0));
    parsed.belts.push_back(Belt("merger_to_craft", "ore_merger", 0, "ai_limiter", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 3);
    REQUIRE(out.nodes[1]->IsMerger());
    const auto* merger = static_cast<const MergerNode*>(out.nodes[1].get());
    REQUIRE(merger->item != nullptr);
    REQUIRE(merger->item->name == "Copper Ore");
}

/// @test   The reported manifold: a multi-output Plastic refinery (Plastic +
///         Heavy Oil Residue) feeds a merger that routes into an AI Limiter
///         (Copper Sheet + Quickwire). The refinery's Plastic belt must be typed
///         Plastic from its output port, and the merger must stay Plastic — not
///         get repainted Copper Sheet by the craft's mismatched input pin.
/// @covers SavImport::BuildGraph multi-output producer seeding + upstream stamp
///         not overriding the producer-resolved item.
TEST_CASE("BuildGraph keeps a refinery-fed merger Plastic against a Copper Sheet consumer", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("plastic_refinery", "Plastic", 0.0f)); // Plastic + Heavy Oil Residue
    parsed.buildings.push_back(Merger("plastic_merger", 100.0f));
    parsed.buildings.push_back(Manufacturer("ai_limiter", "AI Limiter", 200.0f));
    // Refinery's first output port carries Plastic (recipe outs[0]).
    parsed.belts.push_back(Belt("refinery_to_merger", "plastic_refinery", 0, "plastic_merger", 0));
    parsed.belts.push_back(Belt("merger_to_craft", "plastic_merger", 0, "ai_limiter", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 3);
    REQUIRE(out.nodes[1]->IsMerger());
    const auto* merger = static_cast<const MergerNode*>(out.nodes[1].get());
    REQUIRE(merger->item != nullptr);
    REQUIRE(merger->item->name == "Plastic");
}

/// @test   An unloader that ships Coal (cargo arrives by truck) and also burns
///         Coal as vehicle fuel (belt into the fuel inlet) must route that Coal
///         belt to the fuel pin, even though the belt item equals the cargo item.
///         An unloader's cargo never arrives on a belt input, so an input belt is
///         always the fuel inlet.
/// @covers SavImport::BuildGraph unloader input-belt = fuel, same-item case.
TEST_CASE("BuildGraph routes an unloader's input belt to fuel even when fuel equals cargo", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("coal_miner", "Coal", 0.0f));
    parsed.buildings.push_back(TruckStation("station", 100.0f, "Coal", "Coal", true)); // fuel=Coal, cargo=Coal, unloader
    parsed.belts.push_back(Belt("coal_to_station", "coal_miner", 0, "station", 0));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 2);
    Node* station = out.nodes[1].get();
    REQUIRE(station->IsLogistics());
    REQUIRE(!station->ins.empty());
    Pin* fuel_pin = station->ins.back().get();
    REQUIRE(fuel_pin->link != nullptr);
    for (size_t i = 0; i + 1 < station->ins.size(); ++i)
    {
        REQUIRE(station->ins[i]->link == nullptr);
    }
}

/// @test   With connect_vehicle_routes, a route of one loader + one unloader is
///         wired by a plug<->plug route link recorded on both stations'
///         route_links, with no cargo-pin route Link created.
/// @covers SavImport::BuildGraph plug-based vehicle route wiring.
TEST_CASE("BuildGraph wires vehicle routes as plug route links", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("iron_miner", "Iron Ore", -100.0f));
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));
    // Factory belt feeds the loader's cargo input so it has a cargo pin + rate.
    parsed.belts.push_back(Belt("iron_to_loader", "iron_miner", 0, "loader", 0));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader", "guid-loader" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "unloader", "guid-unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[2].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);
    // One route link, indexed on both stations, connecting the two plugs.
    REQUIRE(loader->route_links.size() == 1);
    REQUIRE(unloader->route_links.size() == 1);
    Link* rl = loader->route_links.front();
    REQUIRE(rl == unloader->route_links.front());
    REQUIRE(rl->start == loader->plug.get());
    REQUIRE(rl->end == unloader->plug.get());
    // Plugs carry no Pin::link (route links live in route_links only).
    REQUIRE(loader->plug->link == nullptr);
    REQUIRE(unloader->plug->link == nullptr);
}

/// @test   A (loader, unloader) pair shared by two vehicle routes is wired by a
///         single route link, not one per route (dedup spans all routes).
/// @covers SavImport::BuildGraph vehicle route link dedup across routes.
TEST_CASE("BuildGraph dedups a shared station pair across routes", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader", "guid-loader" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "unloader", "guid-unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[0].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);
    REQUIRE(loader->route_links.size() == 1);
    REQUIRE(unloader->route_links.size() == 1);
}

/// @test   A route containing only loaders (no unloader) creates no route links.
/// @covers SavImport::BuildGraph vehicle route wiring with no matching pair.
TEST_CASE("BuildGraph creates no route link for a loader-only route", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(TruckStation("loader_a", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("loader_b", 100.0f, "Coal", "Iron Ore", false));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader_a", "guid-a" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader_b", "guid-b" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader_a", "loader_b" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    auto* a = dynamic_cast<VehicleStationNode*>(out.nodes[0].get());
    auto* b = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->route_links.empty());
    REQUIRE(b->route_links.empty());
}

/// @test   After wiring a plug route, the importer balances the pool so the
///         loader's incoming cargo rate appears on the unloader's cargo output.
/// @covers SavImport::BuildGraph route pool auto-balance on import.
TEST_CASE("BuildGraph balances cargo rate across an imported route pool", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("iron_miner", "Iron Ore", -100.0f)); // Mk1 normal = 60/min
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));
    parsed.buildings.push_back(Sink("sink", 200.0f));
    parsed.belts.push_back(Belt("iron_to_loader", "iron_miner", 0, "loader", 0));
    parsed.belts.push_back(Belt("unloader_to_sink", "unloader", 0, "sink", 0));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader", "guid-loader" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "unloader", "guid-unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    // nodes follow parsed.buildings insertion order: [0]=miner, [1]=loader, [2]=unloader, [3]=sink
    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[2].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);

    REQUIRE(!loader->ins.empty());
    const Pin* loader_in = loader->ins.front().get();
    REQUIRE(loader_in->item != nullptr);
    REQUIRE(loader_in->current_rate.GetNumerator() != 0);

    bool balanced = false;
    for (const auto& p : unloader->outs)
    {
        if (p->link != nullptr && p->item != nullptr
            && p->current_rate == loader_in->current_rate)
        {
            balanced = true;
        }
    }
    REQUIRE(balanced);
}

/// @test   Truck/train stations import as VehicleStationNode, with mode derived
///       from is_unloader and a vehicle plug whose direction matches the mode.
/// @covers SavImport::BuildGraph station node type + mode.
TEST_CASE("BuildGraph imports stations as VehicleStationNode with mode", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 2);
    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[0].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);
    REQUIRE(loader->mode == VehicleStationNode::Mode::Load);
    REQUIRE(unloader->mode == VehicleStationNode::Mode::Unload);
    REQUIRE(loader->plug->direction == ax::NodeEditor::PinKind::Output);
    REQUIRE(unloader->plug->direction == ax::NodeEditor::PinKind::Input);
    REQUIRE(loader->ins.size() == 3);    // 2 cargo inputs + 1 fuel input
    REQUIRE(loader->outs.size() == 2);   // 2 cargo outputs
    REQUIRE(unloader->ins.size() == 3);  // 2 cargo inputs + 1 fuel input
    REQUIRE(unloader->outs.size() == 2); // 2 cargo outputs
}

/// @test   A generator emitted by the JS wrapper as a normal manufacturer with
///         a Power (...) recipe imports as an ordinary CraftNode. This locks the
///         chosen design: no GeneratorNode or BuildingKind::Generator is needed
///         for this phase.
/// @covers SavImport::BuildGraph generator-as-manufacturer import.
TEST_CASE("BuildGraph imports a resolved generator recipe as a craft node", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("coal_generator", "Power (Coal)", 0.0f));
    parsed.buildings.back().clock = 0.5;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 1);
    REQUIRE(out.nodes[0]->IsCraft());

    const auto* generator = static_cast<const CraftNode*>(out.nodes[0].get());
    REQUIRE(generator->recipe != nullptr);
    REQUIRE(generator->recipe->name == "Power (Coal)");
    REQUIRE(generator->current_rate == FractionalNumber(1, 2));

    auto coal_pin = std::find_if(generator->ins.begin(), generator->ins.end(), [](const std::unique_ptr<Pin>& pin) {
        return pin->item != nullptr && pin->item->name == "Coal";
    });
    auto water_pin = std::find_if(generator->ins.begin(), generator->ins.end(), [](const std::unique_ptr<Pin>& pin) {
        return pin->item != nullptr && pin->item->name == "Water";
    });
    REQUIRE(coal_pin != generator->ins.end());
    REQUIRE(water_pin != generator->ins.end());
    REQUIRE(generator->outs.empty());
}
