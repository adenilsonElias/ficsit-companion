#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Link;
struct Node;

namespace SavImport
{
    enum class BuildingKind
    {
        Unknown,
        Manufacturer,
        Miner,
        Splitter,
        SmartSplitter,
        ProgSplitter,
        Merger,
        Sink,
        Storage,
        IndustrialStorage,
        TruckStation,
        TrainStation,
        DimensionalDepot,
    };

    /// @brief One Satisfactory placed building, parsed from wrapper JSON.
    struct Building
    {
        std::string id;
        BuildingKind kind = BuildingKind::Unknown;
        /// @brief Game-data display name of the recipe (empty if none / unmapped)
        std::string recipe_name;
        /// @brief Game-data display name of the item flowing through (organizers only,
        /// or the extracted resource for extractor buildings)
        std::string item_name;
        /// @brief Overclock multiplier, e.g. 1.0 = 100%
        double clock = 1.0;
        /// @brief Number of somersloops slotted in this machine
        int somersloops = 0;
        /// @brief Extractor specifics. Only meaningful when kind == Miner.
        /// extractor_kind: 0 = unspecified, 1/2/3 = Miner Mk1/2/3,
        /// 4 = Water Extractor, 5 = Oil Extractor.
        int extractor_kind = 0;
        /// @brief "impure" / "normal" / "pure" (empty = unknown).
        std::string extractor_purity;
        /// @brief Max observed save port counts for pass-through logistics.
        int input_count = 0;
        int output_count = 0;
        float x = 0.0f;
        float y = 0.0f;
        /// @brief Belt id connected at each input port, in port-index order. Empty = no belt.
        std::vector<std::string> input_belt_ids;
        std::vector<std::string> output_belt_ids;
        /// @brief Truck/Train station only. Display name of the item currently in
        /// the dedicated FuelInventory (the vehicle fuel) and the cargo inventory
        /// (the shipped item). Empty when the inventory is empty / not a station.
        /// Used to tell a station's fuel belt from its cargo belt, which neither
        /// connector names nor item type can do when both are fuel-capable items.
        std::string fuel_item;
        std::string cargo_item;
        /// @brief Truck/Train station only. True when the station is in unload
        /// mode (mIsInLoadMode == false): cargo arrives by vehicle and leaves on
        /// the output conveyor, so any belt on an input connector is fuel.
        bool is_unloader = false;
    };

    struct Belt
    {
        std::string id;
        std::string item_name; // display name; empty if unknown
        std::string src_building;
        int src_port = 0;
        /// @brief Save-side role of the source connection component.
        /// Currently the only recognized value is "fuel", emitted for the
        /// dedicated fuel inlet on Truck/Train stations.
        std::string src_role;
        /// @brief Direction of the connection component the belt leaves from /
        /// arrives at: "out", "in", "any" (splitter/merger pseudo-port), or
        /// empty for older wrapper output that didn't emit direction. Belts are
        /// one-way (output -> input), so these let the importer reject edges
        /// that would wire the wrong side of a building.
        std::string src_dir;
        std::string dst_building;
        int dst_port = 0;
        std::string dst_role;
        std::string dst_dir;
    };

    /// @brief A truck/train station from the logistics block, used to wire
    /// inter-station vehicle transport edges (see BuildOptions::connect_vehicle_routes).
    struct LogisticsStation
    {
        std::string id;   ///< actor instanceName (matches Building::id)
        std::string guid; ///< route GUID identifying it in vehicle routes
    };

    struct ParseResult
    {
        std::vector<Building> buildings;
        std::vector<Belt> belts;
        std::vector<std::string> warnings;
        /// @brief Truck/train stations from the logistics block (id + route GUID).
        std::vector<LogisticsStation> logistics_stations;
        /// @brief Each entry is one vehicle's route, resolved to an ordered list
        /// of station ids (GUIDs already mapped to Building ids).
        std::vector<std::vector<std::string>> vehicle_routes;
        std::string error;
        bool ok = false;
    };

    /// @brief Parse the JSON emitted by tools/sav_import/wrapper.js
    ParseResult ParseWrapperJson(const std::string& json);

    /// @brief Built node/link graph ready to be wrapped in a GroupNode.
    struct BuildOutput
    {
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<std::unique_ptr<Link>> links;
        std::vector<std::string> warnings;
    };

    enum class LayoutMode
    {
        Compact,
        World,
    };

    /// @brief Scale factor applied to the in-game (x, y) world position to map
    /// onto ImGui Node Editor coordinates. Tunable.
    constexpr float kPositionScale = 0.05f;

    struct BuildOptions
    {
        LayoutMode layout_mode = LayoutMode::Compact;
        float world_spacing_scale = kPositionScale;
        /// @brief If true, wire vehicle routes from the logistics block as
        /// station→station Links (loader output → unloader input), inferring
        /// load/unload direction from each station's connected cargo belts.
        bool connect_vehicle_routes = false;
    };

    /// @brief Build Node/Pin/Link graph from a ParseResult.
    /// Looks up recipes/items by display name in Data::Recipes()/Data::Items().
    /// Wires Links directly between pins (sets pin->link on both ends).
    /// Sets each CraftNode's current_rate from the building's clock multiplier
    /// and num_somersloop, then calls UpdateRate so per-pin rates match.
    bool BuildGraph(const ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        BuildOutput& out,
        std::string& err,
        const BuildOptions& options = BuildOptions());

}
