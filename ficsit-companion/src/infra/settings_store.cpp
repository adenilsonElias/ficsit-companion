#include "infra/settings_store.hpp"
#include "infra/file_store.hpp"
#include "domain/json.hpp"
#include "domain/recipe.hpp"
#include "infra/sav_import.hpp"

#include <cstdlib>
#include <string>

#define WITH_SPOILERS_OPTIONS 0

SettingsStore::SettingsStore(IFileStore& file_store, std::string settings_path)
    : file_store(file_store)
    , settings_path(std::move(settings_path))
{
}

void SettingsStore::Load(Settings& out, const std::vector<const Recipe*>& alt_recipes)
{
    const std::optional<std::string> content = file_store.Load(settings_path);

    Json::Value json = content.has_value() ? Json::Parse(content.value()) : Json::Object();

    // Load all settings values from json
    // Spoilers are disabled since we are not just after a major release anymore
#if WITH_SPOILERS_OPTIONS
    out.show_spoilers = json.contains("show_spoilers") && json["show_spoilers"].get<bool>(); // default false
#else
    out.show_spoilers = true;
#endif
    out.show_somersloop = json.contains("show_somersloop") && json["show_somersloop"].get<bool>(); // default false
    out.unlocked_alts = {};

    for (const Recipe* r : alt_recipes)
    {
        if (r->alternate)
        {
            out.unlocked_alts[r] = json.contains("unlocked_alts") && json["unlocked_alts"].contains(r->name.substr(1)) && json["unlocked_alts"][r->name.substr(1)].get<bool>();
        }
    }
    out.power_equal_clocks = json.contains("power_equal_clocks") && json["power_equal_clocks"].get<bool>(); // default false
    out.show_build_progress = json.contains("show_build_progress") && json["show_build_progress"].get<bool>(); // default false
    out.left_panel_folded = json.contains("left_panel_folded") && json["left_panel_folded"].get<bool>(); // default false
    out.show_resource_flow = json.contains("show_resource_flow") && json["show_resource_flow"].get<bool>(); // default false
    out.show_debug_ids = json.contains("show_debug_ids") && json["show_debug_ids"].get<bool>(); // default false

    out.sav_watch_dir = (json.contains("sav_watch_dir") && json["sav_watch_dir"].is_string())
        ? json["sav_watch_dir"].get_string() : std::string();
    out.sav_watch_world = (json.contains("sav_watch_world") && json["sav_watch_world"].is_string())
        ? json["sav_watch_world"].get_string() : std::string();
    out.sav_watch_enabled = json.contains("sav_watch_enabled") && json["sav_watch_enabled"].get<bool>();
    out.node_executable_path = (json.contains("node_executable_path") && json["node_executable_path"].is_string())
        ? json["node_executable_path"].get_string() : std::string();
    out.sav_import_layout_mode =
        (json.contains("sav_import_layout_mode") && json["sav_import_layout_mode"].is_string() &&
            json["sav_import_layout_mode"].get_string() == "world")
        ? SavImport::LayoutMode::World
        : SavImport::LayoutMode::Compact;
    out.sav_import_world_spacing_scale =
        (json.contains("sav_import_world_spacing_scale") && json["sav_import_world_spacing_scale"].is_number())
        ? json["sav_import_world_spacing_scale"].get<float>()
        : SavImport::kPositionScale;
    if (out.sav_import_world_spacing_scale <= 0.0f)
    {
        out.sav_import_world_spacing_scale = SavImport::kPositionScale;
    }
    out.sav_import_connect_vehicle_routes =
        json.contains("sav_import_connect_vehicle_routes") &&
        json["sav_import_connect_vehicle_routes"].is_bool() &&
        json["sav_import_connect_vehicle_routes"].get<bool>();

    // If no save directory has been configured yet, fall back to the platform-default
    // Satisfactory save folder so the user does not have to type it in manually.
    if (out.sav_watch_dir.empty())
    {
#if defined(_WIN32)
        if (const char* localappdata = std::getenv("LOCALAPPDATA"))
        {
            out.sav_watch_dir = std::string(localappdata) + "\\FactoryGame\\Saved\\SaveGames";
        }
#else
        if (const char* home = std::getenv("HOME"))
        {
            out.sav_watch_dir = std::string(home) + "/.config/Epic/FactoryGame/Saved/SaveGames";
        }
#endif
    }

    if (!content.has_value())
    {
        Save(out);
    }
}

void SettingsStore::Save(const Settings& settings) const
{
    Json::Value serialized;

    // Save all settings values in the json
    serialized["show_spoilers"] = settings.show_spoilers;
    serialized["show_somersloop"] = settings.show_somersloop;

    Json::Object unlocked;
    for (const auto& [r, b] : settings.unlocked_alts)
    {
        // Remove the leading "*" from the alt recipe name
        unlocked[r->name.substr(1)] = b;
    }
    serialized["unlocked_alts"] = unlocked;
    serialized["power_equal_clocks"] = settings.power_equal_clocks;
    serialized["show_build_progress"] = settings.show_build_progress;
    serialized["left_panel_folded"] = settings.left_panel_folded;
    serialized["show_resource_flow"] = settings.show_resource_flow;
    serialized["show_debug_ids"] = settings.show_debug_ids;
    serialized["sav_watch_dir"] = settings.sav_watch_dir;
    serialized["sav_watch_world"] = settings.sav_watch_world;
    serialized["sav_watch_enabled"] = settings.sav_watch_enabled;
    serialized["node_executable_path"] = settings.node_executable_path;
    serialized["sav_import_layout_mode"] =
        settings.sav_import_layout_mode == SavImport::LayoutMode::World ? "world" : "compact";
    serialized["sav_import_world_spacing_scale"] = settings.sav_import_world_spacing_scale;
    serialized["sav_import_connect_vehicle_routes"] = settings.sav_import_connect_vehicle_routes;

    file_store.Save(settings_path, serialized.Dump());
}
