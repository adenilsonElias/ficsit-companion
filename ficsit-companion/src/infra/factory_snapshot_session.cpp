#include "infra/factory_snapshot_session.hpp"

#include "domain/json.hpp"

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

    if (flow_filter < 0 || flow_filter > 2) flow_filter = 0;
    node_font_scale = std::clamp(node_font_scale, 0.5f, 3.0f);
    icon_scale = std::clamp(icon_scale, 0.5f, 4.0f);
}
