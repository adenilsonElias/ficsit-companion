#pragma once

#include <imgui.h>

#include <string>

#include "domain/vehicle/vehicle_map_camera.hpp" // kDefaultZoom (single source of truth)

/// @brief Serializable snapshot of the Vehicle Map tool's persisted state:
/// save-watch config, camera pan/zoom, selections, filters and layer toggles.
///
/// A plain data carrier with JSON (de)serialization, mirroring SettingsStore /
/// SessionSerializer. The host app gathers its live state (camera + members)
/// into this struct to save, and scatters a parsed struct back to load. File
/// I/O stays in the app.
struct VehicleMapSession
{
    std::string node_executable_path;
    std::string sav_watch_dir;
    std::string sav_watch_world;
    bool sav_watch_enabled = false;
    std::string last_sav_path;

    ImVec2 pan{ 0.0f, 0.0f };
    float zoom = VehicleMapCamera::kDefaultZoom;

    std::string sel_station;
    std::string sel_vehicle;
    std::string sel_item;
    std::string search;
    unsigned int type_mask = 0xFFFFFFFFu;

    struct Layers { bool roads = true, rails = true, vehicles = true, stations = true, labels = true; } layers;
    bool color_by_item = false;

    /// @brief Serialize to a pretty-printed JSON string.
    std::string Serialize() const;
    /// @brief Parse from JSON; unknown / missing / mistyped keys keep current
    /// values, and a non-positive zoom is clamped to kDefaultZoom. Malformed
    /// JSON leaves the struct untouched.
    void Deserialize(const std::string& json);
};
