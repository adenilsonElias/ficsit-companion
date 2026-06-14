#pragma once

#include <imgui.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "domain/vehicle_map.hpp"

/// @brief Pure filter / highlight queries over a VehicleMap::Model.
///
/// No ImGui rendering or app state: every input is passed explicitly so the
/// matching and highlight logic is unit-testable against a fabricated model.
namespace VehicleMapQuery
{
    /// @brief Case-insensitive substring test. Empty needle always matches.
    bool ContainsCI(const std::string& haystack, const std::string& needle);

    /// @brief Free-text filter: match on station name or any cargo item name.
    bool StationPassesFilter(const VehicleMap::Station& st, const std::string& search);

    /// @brief Type-mask + free-text filter for a vehicle (matches name or type name).
    bool VehiclePassesFilter(const VehicleMap::Vehicle& ve, const std::string& search,
                             unsigned int type_mask);

    /// @brief Highlighted / dimmed entities for the current selection or item query.
    struct Highlight
    {
        bool active = false;
        std::unordered_set<std::string> stations; // highlighted station ids
        std::unordered_set<std::string> vehicles; // highlighted vehicle ids
        std::vector<std::vector<ImVec2>> routes;  // route polylines (world space)
    };

    /// @brief Compute the highlight set for a selected vehicle, station, or item
    /// query (checked in that priority order). Empty selections yield an inactive
    /// highlight.
    Highlight ComputeHighlight(const VehicleMap::Model& model,
                               const std::string& sel_vehicle,
                               const std::string& sel_station,
                               const std::string& sel_item);
}
