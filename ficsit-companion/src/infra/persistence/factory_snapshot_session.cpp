#include "infra/persistence/factory_snapshot_session.hpp"

#include "domain/core/json.hpp"

#include <algorithm>
#include <exception>

std::string FactorySnapshotSession::Serialize() const
{
    Json::Value v;
    v["version"] = 1;
    v["flow_filter"] = flow_filter;
    v["flow_search"] = flow_search;
    v["world_layout"] = world_layout;
    v["node_font_scale"] = node_font_scale;
    v["icon_scale"] = icon_scale;
    v["collapsed_font_scale"] = collapsed_font_scale;
    v["hide_logistics_enabled"] = hide_logistics_enabled;
    v["hide_game_splitters"] = hide_game_splitters;
    v["hide_custom_splitters"] = hide_custom_splitters;
    v["hide_mergers"] = hide_mergers;
    v["hide_logistics_nodes"] = hide_logistics_nodes;
    Json::Array hidden_items;
    for (const std::string& name : hidden_production_items)
        hidden_items.push_back(Json::Value(name));
    v["hidden_production_items"] = hidden_items;
    return v.Dump(2);
}

void FactorySnapshotSession::Deserialize(const std::string& json)
{
    Json::Value v;
    try { v = Json::Parse(json); }
    catch (const std::exception&) { return; }
    if (!v.is_object()) return;

    if (v.contains("flow_filter") && v["flow_filter"].is_number()) flow_filter = v["flow_filter"].get<int>();
    if (v.contains("flow_search") && v["flow_search"].is_string()) flow_search = v["flow_search"].get_string();
    if (v.contains("world_layout") && v["world_layout"].is_bool()) world_layout = v["world_layout"].get<bool>();
    if (v.contains("node_font_scale") && v["node_font_scale"].is_number()) node_font_scale = v["node_font_scale"].get<float>();
    if (v.contains("icon_scale") && v["icon_scale"].is_number()) icon_scale = v["icon_scale"].get<float>();
    if (v.contains("collapsed_font_scale") && v["collapsed_font_scale"].is_number()) collapsed_font_scale = v["collapsed_font_scale"].get<float>();
    if (v.contains("hide_logistics_enabled") && v["hide_logistics_enabled"].is_bool()) hide_logistics_enabled = v["hide_logistics_enabled"].get<bool>();
    if (v.contains("hide_game_splitters") && v["hide_game_splitters"].is_bool()) hide_game_splitters = v["hide_game_splitters"].get<bool>();
    if (v.contains("hide_custom_splitters") && v["hide_custom_splitters"].is_bool()) hide_custom_splitters = v["hide_custom_splitters"].get<bool>();
    if (v.contains("hide_mergers") && v["hide_mergers"].is_bool()) hide_mergers = v["hide_mergers"].get<bool>();
    if (v.contains("hide_logistics_nodes") && v["hide_logistics_nodes"].is_bool()) hide_logistics_nodes = v["hide_logistics_nodes"].get<bool>();
    if (v.contains("hidden_production_items") && v["hidden_production_items"].is_array())
    {
        hidden_production_items.clear();
        for (const auto& e : v["hidden_production_items"].get_array())
            if (e.is_string()) hidden_production_items.push_back(e.get_string());
    }

    if (flow_filter < 0 || flow_filter > 2) flow_filter = 0;
    node_font_scale = std::clamp(node_font_scale, 0.5f, 3.0f);
    icon_scale = std::clamp(icon_scale, 0.5f, 4.0f);
    collapsed_font_scale = std::clamp(collapsed_font_scale, 0.5f, 3.0f);
}
