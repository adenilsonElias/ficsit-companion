#include "infra/saveimport/save_source.hpp"

#include "domain/core/json.hpp"

#include <exception>
#include <filesystem>

SavImport::BuildOptions SaveSource::ToBuildOptions() const
{
    SavImport::BuildOptions options;
    options.layout_mode = layout_mode;
    options.world_spacing_scale = world_spacing_scale;
    options.connect_vehicle_routes = connect_vehicle_routes;
    return options;
}

std::string SaveSource::Serialize() const
{
    Json::Value v;
    v["version"] = 1;
    v["node_executable_path"] = node_executable_path;
    v["sav_watch_dir"] = sav_watch_dir;
    v["sav_watch_world"] = sav_watch_world;
    v["sav_watch_enabled"] = sav_watch_enabled;
    v["last_sav_path"] = last_sav_path;
    v["sav_import_layout_mode"] = (layout_mode == SavImport::LayoutMode::World) ? 1 : 0;
    v["sav_import_world_spacing_scale"] = world_spacing_scale;
    v["sav_import_connect_vehicle_routes"] = connect_vehicle_routes;
    return v.Dump(2);
}

void SaveSource::Deserialize(const std::string& json)
{
    Json::Value v;
    try { v = Json::Parse(json); }
    catch (const std::exception&) { return; }
    if (!v.is_object()) return;

    auto str = [&](const char* k, std::string& out) { if (v.contains(k) && v[k].is_string()) out = v[k].get_string(); };
    auto boolean = [&](const char* k, bool& out) { if (v.contains(k) && v[k].is_bool()) out = v[k].get<bool>(); };

    str("node_executable_path", node_executable_path);
    str("sav_watch_dir", sav_watch_dir);
    str("sav_watch_world", sav_watch_world);
    boolean("sav_watch_enabled", sav_watch_enabled);
    str("last_sav_path", last_sav_path);

    if (v.contains("sav_import_layout_mode") && v["sav_import_layout_mode"].is_number())
    {
        layout_mode = v["sav_import_layout_mode"].get_number<int>() == 1
            ? SavImport::LayoutMode::World
            : SavImport::LayoutMode::Compact;
    }
    if (v.contains("sav_import_world_spacing_scale") && v["sav_import_world_spacing_scale"].is_number())
    {
        const float scale = v["sav_import_world_spacing_scale"].get_number<float>();
        if (scale > 0.0f) world_spacing_scale = scale;
    }
    boolean("sav_import_connect_vehicle_routes", connect_vehicle_routes);
}

std::optional<std::string> SaveSource::FindLatestSav() const
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::is_directory(sav_watch_dir, ec))
    {
        return std::nullopt;
    }

    fs::path best;
    fs::file_time_type best_time{};
    bool found = false;
    for (const auto& entry : fs::directory_iterator(sav_watch_dir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sav") continue;

        if (!sav_watch_world.empty())
        {
            const std::string filename = entry.path().filename().string();
            const std::string needle = sav_watch_world + "_";
            if (filename.size() < needle.size() ||
                filename.compare(0, needle.size(), needle) != 0)
            {
                continue;
            }
        }

        std::error_code time_ec;
        const auto time = fs::last_write_time(entry.path(), time_ec);
        if (time_ec) continue;

        if (!found || time > best_time)
        {
            best = entry.path();
            best_time = time;
            found = true;
        }
    }

    return found ? std::optional<std::string>(best.string()) : std::nullopt;
}
