#include "domain/vehicle_map_query.hpp"

#include <algorithm>
#include <cctype>

namespace
{
    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    unsigned int TypeBit(VehicleMap::VehicleType t)
    {
        return 1u << static_cast<unsigned int>(t);
    }
}

namespace VehicleMapQuery
{
    bool ContainsCI(const std::string& haystack, const std::string& needle)
    {
        if (needle.empty()) return true;
        const std::string h = ToLower(haystack);
        const std::string n = ToLower(needle);
        return h.find(n) != std::string::npos;
    }

    bool StationPassesFilter(const VehicleMap::Station& st, const std::string& search)
    {
        if (search.empty()) return true;
        if (ContainsCI(st.name, search)) return true;
        for (const std::string& it : st.item_names) if (ContainsCI(it, search)) return true;
        return false;
    }

    bool VehiclePassesFilter(const VehicleMap::Vehicle& ve, const std::string& search,
                             unsigned int type_mask)
    {
        if ((type_mask & TypeBit(ve.type)) == 0) return false;
        if (search.empty()) return true;
        return ContainsCI(ve.name, search) || ContainsCI(VehicleMap::VehicleTypeName(ve.type), search);
    }

    Highlight ComputeHighlight(const VehicleMap::Model& model,
                               const std::string& sel_vehicle,
                               const std::string& sel_station,
                               const std::string& sel_item)
    {
        Highlight h;

        auto route_polyline = [&](const VehicleMap::Vehicle& ve) {
            std::vector<ImVec2> poly;
            for (const std::string& sid : ve.station_ids)
            {
                auto it = model.station_index.find(sid);
                if (it != model.station_index.end()) poly.push_back(model.stations[it->second].pos);
            }
            if (poly.size() >= 2) h.routes.push_back(std::move(poly));
        };

        if (!sel_vehicle.empty())
        {
            auto it = model.vehicle_index.find(sel_vehicle);
            if (it != model.vehicle_index.end())
            {
                const VehicleMap::Vehicle& ve = model.vehicles[it->second];
                h.active = true;
                h.vehicles.insert(ve.id);
                for (const std::string& sid : ve.station_ids) h.stations.insert(sid);
                route_polyline(ve);
                return h;
            }
        }
        if (!sel_station.empty())
        {
            auto it = model.station_index.find(sel_station);
            if (it != model.station_index.end())
            {
                const VehicleMap::Station& st = model.stations[it->second];
                h.active = true;
                h.stations.insert(st.id);
                for (const std::string& vid : st.vehicle_ids)
                {
                    h.vehicles.insert(vid);
                    auto vit = model.vehicle_index.find(vid);
                    if (vit != model.vehicle_index.end()) route_polyline(model.vehicles[vit->second]);
                }
                return h;
            }
        }
        if (!sel_item.empty())
        {
            auto it = model.item_index.find(ToLower(sel_item));
            if (it != model.item_index.end())
            {
                h.active = true;
                for (size_t si : it->second.stations) h.stations.insert(model.stations[si].id);
                for (size_t vi : it->second.vehicles)
                {
                    h.vehicles.insert(model.vehicles[vi].id);
                    route_polyline(model.vehicles[vi]);
                }
            }
        }
        return h;
    }
}
