#pragma once

#include <optional>
#include <string>

#include "infra/saveimport/sav_import.hpp"

/// @brief Shared, persisted configuration for loading a Satisfactory `.sav`.
/// A plain data carrier with JSON (de)serialization, mirroring VehicleMapSession.
/// Owned by the AppHost so a single load can feed every tool. File I/O and the
/// actual subprocess run stay outside this struct.
struct SaveSource
{
    std::string node_executable_path;
    std::string sav_watch_dir;
    std::string sav_watch_world;
    bool sav_watch_enabled = false;
    std::string last_sav_path;

    // Import options shared by every tool. These used to live in the modeler's
    // per-tool "Save Import" settings; they now drive all three tools from the
    // single global load bar so the import behaves identically everywhere.
    SavImport::LayoutMode layout_mode = SavImport::LayoutMode::Compact;
    float world_spacing_scale = SavImport::kPositionScale;
    bool connect_vehicle_routes = false;

    /// @brief Build the SavImport options every tool uses when importing.
    SavImport::BuildOptions ToBuildOptions() const;

    /// @brief Serialize to a pretty-printed JSON string.
    std::string Serialize() const;
    /// @brief Parse from JSON; unknown / missing / mistyped keys keep current
    /// values. Malformed JSON leaves the struct untouched.
    void Deserialize(const std::string& json);

    /// @brief Return the newest `.sav` in sav_watch_dir, filtered by
    /// sav_watch_world when set. Empty means no matching file was found.
    std::optional<std::string> FindLatestSav() const;
};
