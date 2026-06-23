#pragma once

#include <imgui.h>

#include <string>
#include <unordered_map>
#include <vector>

struct Item;

/// @brief Flat logistics data model for the Vehicle Map tool.
///
/// Independent of node.hpp / sav_import.hpp: it consumes only the additive
/// `logistics` block emitted by tools/sav_import/wrapper.js and produces a model
/// the map renderer can draw and query. Cross-links (station<->vehicle) and the
/// item / orphan indices are resolved once at parse time.
namespace VehicleMap
{
    enum class StationKind { Truck, Train };
    enum class VehicleType { Truck, Tractor, Explorer, CyberWagon, Train, Unknown };
    enum class PathKind { Road, Rail };

    struct Station
    {
        std::string id;
        std::string name;
        /// @brief Route GUID ("a_b_c_d") identifying this station in vehicle routes.
        std::string guid;
        StationKind kind = StationKind::Truck;
        /// @brief Physical road-network id (coarse; not a per-vehicle route).
        int network_id = -1;
        ImVec2 pos{ 0.0f, 0.0f };
        /// @brief Cargo item display names (raw from wrapper; covers items missing
        /// from game data). Always populated.
        std::vector<std::string> item_names;
        /// @brief Resolved Item* for each name found in game data (no nullptrs).
        std::vector<const Item*> items;
        /// @brief Vehicle ids that serve this station (resolved from routes + docking).
        std::vector<std::string> vehicle_ids;
    };

    struct Vehicle
    {
        std::string id;
        std::string name;
        VehicleType type = VehicleType::Unknown;
        int network_id = -1;
        ImVec2 pos{ 0.0f, 0.0f };
        const Item* fuel = nullptr;
        std::string fuel_name;
        bool autopilot = false;
        /// @brief Ordered route stop GUIDs (raw).
        std::vector<std::string> route_guids;
        /// @brief Ordered route stops resolved to station ids (unknown GUIDs dropped).
        std::vector<std::string> station_ids;
    };

    /// @brief One road/rail path segment — used purely for drawing the background
    /// road map. (Vehicle routes are reconstructed from station stops, not these.)
    struct Segment
    {
        std::string id;
        PathKind kind = PathKind::Road;
        int network_id = -1;
        std::vector<ImVec2> waypoints;
    };

    /// @brief Entities that handle a given item (indices into Model vectors).
    struct ItemRefs
    {
        std::vector<size_t> stations;
        std::vector<size_t> vehicles;
    };

    struct Model
    {
        std::vector<Station> stations;
        std::vector<Vehicle> vehicles;
        std::vector<Segment> segments;

        // --- Derived indices (built by ParseLogisticsJson) ---
        std::unordered_map<std::string, size_t> station_index;   ///< station id  -> index
        std::unordered_map<std::string, size_t> vehicle_index;   ///< vehicle id  -> index
        std::unordered_map<std::string, size_t> guid_to_station; ///< route GUID  -> station index
        /// @brief lowercased item display name -> stations/vehicles handling it.
        std::unordered_map<std::string, ItemRefs> item_index;
        /// @brief Distinct item display names (original case), sorted — for the filter combo.
        std::vector<std::string> item_names_sorted;
        /// @brief Stations served by no vehicle.
        std::vector<size_t> orphan_stations;
        /// @brief Vehicles with an empty / unresolvable route.
        std::vector<size_t> orphan_vehicles;

        // World-space bounding box over every drawn entity (for "fit all").
        ImVec2 world_min{ 0.0f, 0.0f };
        ImVec2 world_max{ 0.0f, 0.0f };
        bool has_bounds = false;

        std::string error;
        bool ok = false;

        bool Empty() const { return stations.empty() && vehicles.empty() && segments.empty(); }
    };

    /// @brief Parse the `logistics` block of wrapper.js output into a Model.
    /// Always returns a Model; on failure `ok == false` and `error` is set.
    Model ParseLogisticsJson(const std::string& wrapper_json);

    /// @brief Pull the wrapper's logistics-related warning lines (those whose
    /// text contains "logistics") from its JSON output, for diagnostics.
    /// Returns empty on malformed JSON or a missing/!array `warnings` field.
    std::vector<std::string> ExtractLogisticsWarnings(const std::string& wrapper_json);

    /// @brief Human label for a vehicle type (e.g. "Tractor").
    const char* VehicleTypeName(VehicleType t);
}
