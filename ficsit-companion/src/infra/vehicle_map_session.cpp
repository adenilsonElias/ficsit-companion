#include "infra/vehicle_map_session.hpp"

#include "domain/json.hpp"

#include <exception>

std::string VehicleMapSession::Serialize() const
{
    Json::Value v;
    v["version"] = 1;
    v["node_executable_path"] = node_executable_path;
    v["sav_watch_dir"] = sav_watch_dir;
    v["sav_watch_world"] = sav_watch_world;
    v["sav_watch_enabled"] = sav_watch_enabled;
    v["last_sav_path"] = last_sav_path;
    v["pan_x"] = pan.x;
    v["pan_y"] = pan.y;
    v["zoom"] = zoom;
    v["sel_station"] = sel_station;
    v["sel_vehicle"] = sel_vehicle;
    v["sel_item"] = sel_item;
    v["search"] = search;
    v["type_mask"] = type_mask;
    v["layer_roads"] = layers.roads;
    v["layer_rails"] = layers.rails;
    v["layer_vehicles"] = layers.vehicles;
    v["layer_stations"] = layers.stations;
    v["layer_labels"] = layers.labels;
    v["color_by_item"] = color_by_item;
    return v.Dump(2);
}

void VehicleMapSession::Deserialize(const std::string& json)
{
    Json::Value v;
    try { v = Json::Parse(json); }
    catch (const std::exception&) { return; }
    if (!v.is_object()) return;

    auto str = [&](const char* k, std::string& out) { if (v.contains(k) && v[k].is_string()) out = v[k].get_string(); };
    auto boolean = [&](const char* k, bool& out) { if (v.contains(k) && v[k].is_bool()) out = v[k].get<bool>(); };
    auto flt = [&](const char* k, float& out) { if (v.contains(k) && v[k].is_number()) out = v[k].get<float>(); };

    str("node_executable_path", node_executable_path);
    str("sav_watch_dir", sav_watch_dir);
    str("sav_watch_world", sav_watch_world);
    boolean("sav_watch_enabled", sav_watch_enabled);
    str("last_sav_path", last_sav_path);
    flt("pan_x", pan.x);
    flt("pan_y", pan.y);
    flt("zoom", zoom);
    str("sel_station", sel_station);
    str("sel_vehicle", sel_vehicle);
    str("sel_item", sel_item);
    str("search", search);
    if (v.contains("type_mask") && v["type_mask"].is_number()) type_mask = v["type_mask"].get<unsigned int>();
    boolean("layer_roads", layers.roads);
    boolean("layer_rails", layers.rails);
    boolean("layer_vehicles", layers.vehicles);
    boolean("layer_stations", layers.stations);
    boolean("layer_labels", layers.labels);
    boolean("color_by_item", color_by_item);
    if (zoom <= 0.0f) zoom = VehicleMapCamera::kDefaultZoom;
}
