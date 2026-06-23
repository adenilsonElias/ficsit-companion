#include "domain/vehicle/vehicle_map.hpp"

#include "domain/gamedata/game_data.hpp"
#include "domain/core/json.hpp"
#include "domain/gamedata/recipe.hpp" // struct Item

#include <algorithm>
#include <cctype>
#include <exception>
#include <unordered_set>

namespace VehicleMap
{
    namespace
    {
        std::string ToLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        StationKind ParseStationKind(const std::string& s)
        {
            return s == "train" ? StationKind::Train : StationKind::Truck;
        }

        VehicleType ParseVehicleType(const std::string& s)
        {
            if (s == "tractor")    return VehicleType::Tractor;
            if (s == "explorer")   return VehicleType::Explorer;
            if (s == "cyberwagon") return VehicleType::CyberWagon;
            if (s == "train")      return VehicleType::Train;
            if (s == "truck")      return VehicleType::Truck;
            return VehicleType::Unknown;
        }

        // Read pos[0], pos[1] from a JSON array if present.
        ImVec2 ReadPos(const Json::Value& v)
        {
            ImVec2 p{ 0.0f, 0.0f };
            if (v.is_array())
            {
                const auto& arr = v.get_array();
                if (arr.size() >= 1 && arr[0].is_number()) p.x = arr[0].get<float>();
                if (arr.size() >= 2 && arr[1].is_number()) p.y = arr[1].get<float>();
            }
            return p;
        }

        void ReadStringArray(const Json::Value& v, std::vector<std::string>& out)
        {
            if (!v.is_array()) return;
            for (const auto& e : v.get_array())
            {
                if (e.is_string()) out.push_back(e.get_string());
            }
        }

        // Case-insensitive item lookup against Data::Items() (display-name keyed).
        const Item* LookupItem(const std::string& name)
        {
            if (name.empty()) return nullptr;
            const auto& items = Data::Items();
            // Fast path: exact display-name key.
            auto it = items.find(name);
            if (it != items.end()) return it->second.get();
            // Slow path: case-insensitive scan (display names are few hundred).
            const std::string lower = ToLower(name);
            for (const auto& [key, item] : items)
            {
                if (ToLower(key) == lower) return item.get();
            }
            return nullptr;
        }

        void ExpandBounds(Model& m, const ImVec2& p)
        {
            if (!m.has_bounds)
            {
                m.world_min = p;
                m.world_max = p;
                m.has_bounds = true;
                return;
            }
            m.world_min.x = std::min(m.world_min.x, p.x);
            m.world_min.y = std::min(m.world_min.y, p.y);
            m.world_max.x = std::max(m.world_max.x, p.x);
            m.world_max.y = std::max(m.world_max.y, p.y);
        }
    }

    const char* VehicleTypeName(VehicleType t)
    {
        switch (t)
        {
        case VehicleType::Truck:      return "Truck";
        case VehicleType::Tractor:    return "Tractor";
        case VehicleType::Explorer:   return "Explorer";
        case VehicleType::CyberWagon: return "Cyber Wagon";
        case VehicleType::Train:      return "Train";
        default:                      return "Vehicle";
        }
    }

    std::vector<std::string> ExtractLogisticsWarnings(const std::string& wrapper_json)
    {
        std::vector<std::string> warnings;
        try
        {
            Json::Value root = Json::Parse(wrapper_json);
            if (root.is_object() && root.contains("warnings") && root["warnings"].is_array())
            {
                for (const auto& w : root["warnings"].get_array())
                {
                    if (w.is_string() && w.get_string().find("logistics") != std::string::npos)
                    {
                        warnings.push_back(w.get_string());
                    }
                }
            }
        }
        catch (const std::exception&) {}
        return warnings;
    }

    Model ParseLogisticsJson(const std::string& wrapper_json)
    {
        Model model;

        Json::Value root;
        try
        {
            root = Json::Parse(wrapper_json);
        }
        catch (const std::exception& e)
        {
            model.error = std::string("Failed to parse wrapper JSON: ") + e.what();
            return model;
        }

        if (root.is_null() || !root.is_object())
        {
            model.error = "Wrapper JSON has no root object";
            return model;
        }
        if (!root.contains("logistics") || !root["logistics"].is_object())
        {
            model.error = "Save contains no logistics data (no vehicles/stations found)";
            return model;
        }
        const Json::Value& L = root["logistics"];

        // --- Stations ---
        if (L.contains("stations") && L["stations"].is_array())
        {
            for (const auto& s : L["stations"].get_array())
            {
                Station st;
                if (s.contains("id") && s["id"].is_string())   st.id = s["id"].get_string();
                if (s.contains("name") && s["name"].is_string()) st.name = s["name"].get_string();
                if (s.contains("guid") && s["guid"].is_string()) st.guid = s["guid"].get_string();
                if (s.contains("kind") && s["kind"].is_string()) st.kind = ParseStationKind(s["kind"].get_string());
                if (s.contains("network_id") && s["network_id"].is_number()) st.network_id = s["network_id"].get<int>();
                if (s.contains("pos")) st.pos = ReadPos(s["pos"]);
                if (s.contains("items")) ReadStringArray(s["items"], st.item_names);
                for (const std::string& name : st.item_names)
                {
                    if (const Item* item = LookupItem(name)) st.items.push_back(item);
                }
                if (st.id.empty()) continue;
                model.stations.push_back(std::move(st));
            }
        }

        // --- Vehicles ---
        if (L.contains("vehicles") && L["vehicles"].is_array())
        {
            for (const auto& v : L["vehicles"].get_array())
            {
                Vehicle ve;
                if (v.contains("id") && v["id"].is_string())   ve.id = v["id"].get_string();
                if (v.contains("name") && v["name"].is_string()) ve.name = v["name"].get_string();
                if (v.contains("type") && v["type"].is_string()) ve.type = ParseVehicleType(v["type"].get_string());
                if (v.contains("network_id") && v["network_id"].is_number()) ve.network_id = v["network_id"].get<int>();
                if (v.contains("pos")) ve.pos = ReadPos(v["pos"]);
                if (v.contains("autopilot") && v["autopilot"].is_bool()) ve.autopilot = v["autopilot"].get<bool>();
                if (v.contains("fuel") && v["fuel"].is_string())
                {
                    ve.fuel_name = v["fuel"].get_string();
                    ve.fuel = LookupItem(ve.fuel_name);
                }
                if (v.contains("route_guids")) ReadStringArray(v["route_guids"], ve.route_guids);
                if (ve.id.empty()) continue;
                model.vehicles.push_back(std::move(ve));
            }
        }

        // --- Path segments ---
        if (L.contains("paths") && L["paths"].is_array())
        {
            for (const auto& p : L["paths"].get_array())
            {
                Segment seg;
                if (p.contains("id") && p["id"].is_string()) seg.id = p["id"].get_string();
                if (p.contains("kind") && p["kind"].is_string())
                {
                    seg.kind = (p["kind"].get_string() == "rail") ? PathKind::Rail : PathKind::Road;
                }
                if (p.contains("network_id") && p["network_id"].is_number()) seg.network_id = p["network_id"].get<int>();
                if (p.contains("waypoints") && p["waypoints"].is_array())
                {
                    for (const auto& wp : p["waypoints"].get_array())
                    {
                        if (!wp.is_array()) continue;
                        const auto& a = wp.get_array();
                        if (a.size() < 2 || !a[0].is_number() || !a[1].is_number()) continue;
                        seg.waypoints.push_back(ImVec2(a[0].get<float>(), a[1].get<float>()));
                    }
                }
                if (seg.waypoints.size() >= 2) model.segments.push_back(std::move(seg));
            }
        }

        // --- Derived indices ---
        for (size_t i = 0; i < model.stations.size(); ++i)
        {
            model.station_index[model.stations[i].id] = i;
            const std::string& guid = model.stations[i].guid;
            if (!guid.empty()) model.guid_to_station[guid] = i;
        }
        for (size_t i = 0; i < model.vehicles.size(); ++i)
        {
            model.vehicle_index[model.vehicles[i].id] = i;
        }

        // Resolve each vehicle's ordered route stops, and register the reverse
        // (station -> serving vehicles) link.
        for (size_t vi = 0; vi < model.vehicles.size(); ++vi)
        {
            Vehicle& ve = model.vehicles[vi];
            std::unordered_set<std::string> seen_station;
            for (const std::string& guid : ve.route_guids)
            {
                auto it = model.guid_to_station.find(guid);
                if (it == model.guid_to_station.end()) continue;
                Station& st = model.stations[it->second];
                ve.station_ids.push_back(st.id);
                if (seen_station.insert(st.id).second)
                {
                    // Avoid duplicate vehicle entries on a station whose route lists
                    // it more than once.
                    if (std::find(st.vehicle_ids.begin(), st.vehicle_ids.end(), ve.id) == st.vehicle_ids.end())
                    {
                        st.vehicle_ids.push_back(ve.id);
                    }
                }
            }
        }

        // Item index: stations that hold an item, plus every vehicle that serves
        // such a station (so "show item X" lights up its whole logistics network).
        for (size_t si = 0; si < model.stations.size(); ++si)
        {
            const Station& st = model.stations[si];
            std::unordered_set<std::string> names_lower;
            for (const std::string& name : st.item_names)
            {
                const std::string key = ToLower(name);
                if (!names_lower.insert(key).second) continue;
                ItemRefs& refs = model.item_index[key];
                refs.stations.push_back(si);
                for (const std::string& vid : st.vehicle_ids)
                {
                    auto vit = model.vehicle_index.find(vid);
                    if (vit == model.vehicle_index.end()) continue;
                    if (std::find(refs.vehicles.begin(), refs.vehicles.end(), vit->second) == refs.vehicles.end())
                    {
                        refs.vehicles.push_back(vit->second);
                    }
                }
            }
        }

        // Distinct display names (original case) sorted, for the filter combo.
        {
            std::unordered_set<std::string> seen;
            for (const Station& st : model.stations)
            {
                for (const std::string& name : st.item_names)
                {
                    if (seen.insert(ToLower(name)).second) model.item_names_sorted.push_back(name);
                }
            }
            std::sort(model.item_names_sorted.begin(), model.item_names_sorted.end(),
                [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); });
        }

        // Orphans.
        for (size_t si = 0; si < model.stations.size(); ++si)
        {
            if (model.stations[si].vehicle_ids.empty()) model.orphan_stations.push_back(si);
        }
        for (size_t vi = 0; vi < model.vehicles.size(); ++vi)
        {
            if (model.vehicles[vi].station_ids.empty()) model.orphan_vehicles.push_back(vi);
        }

        // World bounds over everything we will draw.
        for (const Station& st : model.stations) ExpandBounds(model, st.pos);
        for (const Vehicle& ve : model.vehicles) ExpandBounds(model, ve.pos);
        for (const Segment& seg : model.segments)
        {
            for (const ImVec2& w : seg.waypoints) ExpandBounds(model, w);
        }

        model.ok = true;
        return model;
    }
}
