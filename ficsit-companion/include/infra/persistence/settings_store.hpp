#pragma once
#include <map>
#include <string>
#include <vector>
#include "infra/saveimport/sav_import.hpp"

struct Recipe;
class IFileStore;

/// @brief All settings to customize app behaviour
struct Settings {
    /// @brief If true, recipes marked as spoiler will be added to the list
    bool show_spoilers = false;
    /// @brief If true, somersloop override will be displayed in the nodes
    bool show_somersloop = false;
    /// @brief For each alt recipes, stores wether or not it's been unlocked yet
    std::map<const Recipe*, bool> unlocked_alts = {};
    /// @brief If true, will display power info with equal clocks on all machines in a node
    /// If false, it will compute the power for N machines at 100% + an underclocked machine
    bool power_equal_clocks = true;
    /// @brief If true, build progress bar and checkbox on craft nodes will be displayed
    bool show_build_progress = false;
    /// @brief If true, the left panel will be minimized
    bool left_panel_folded = false;
    /// @brief If true, the Resource Flow window is open.
    bool show_resource_flow = false;
    /// @brief If true, graph nodes, pins, and links expose their editor ids in the UI.
    bool show_debug_ids = false;
    /// @brief Folder watched for new .sav files
    std::string sav_watch_dir;
    /// @brief World name prefix to filter on (empty = all worlds)
    std::string sav_watch_world;
    /// @brief Whether the SaveWatcher is currently running
    bool sav_watch_enabled = false;
    /// @brief Path to the node executable used to run wrapper.js (empty = use PATH)
    std::string node_executable_path;
    SavImport::LayoutMode sav_import_layout_mode = SavImport::LayoutMode::Compact;
    float sav_import_world_spacing_scale = SavImport::kPositionScale;
    /// @brief If true, .sav import wires vehicle routes as station->station links.
    bool sav_import_connect_vehicle_routes = false;
};

/// @brief Loads/saves Settings as JSON through an IFileStore. Pure of UI.
class SettingsStore
{
public:
    SettingsStore(IFileStore& file_store, std::string settings_path);
    /// @brief Populate `out` from the settings file. `alt_recipes` is the list
    /// of recipes whose alternate-unlock flags should be read (Data::Recipes()).
    /// Writes defaults back via Save() if the file was absent.
    void Load(Settings& out, const std::vector<const Recipe*>& alt_recipes);
    void Save(const Settings& settings) const;
private:
    IFileStore& file_store;
    std::string settings_path;
};
