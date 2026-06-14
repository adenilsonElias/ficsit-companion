#include "infra/sav_import.hpp"

#include "domain/fractional_number.hpp"
#include "domain/game_data.hpp"
#include "domain/json.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"
#include "app/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace SavImport
{
    namespace
    {
        std::string ToLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // The dedicated fuel inlet on a Truck/Train station is the last input
        // pin and is isolated from the cargo flow: it must not feed the station's
        // outputs nor contribute to the throughput rate. Mirrors IsFuelPin in
        // production_app.cpp.
        bool IsStationFuelPin(const Node* node, const Pin* pin)
        {
            if (node == nullptr || !node->IsLogistics() || node->ins.empty()) return false;
            const LogisticsNode* l = static_cast<const LogisticsNode*>(node);
            if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
                l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
            return pin == node->ins.back().get();
        }

        // A belt the wrapper flagged as feeding (or leaving) a station's
        // dedicated fuel inlet. Fuel belts carry a fuel item that is independent
        // of the station's cargo, so item inference must never mix the two.
        bool IsFuelBelt(const Belt& belt)
        {
            return belt.src_role == "fuel" || belt.dst_role == "fuel";
        }

        // Lazy-init cache keyed by lowercase display name.
        // Rebuilt only if Data::Recipes()/Items() pointer-identity changes,
        // which in practice happens once at startup.
        struct LookupCache
        {
            std::mutex mutex;
            std::unordered_map<std::string, const Recipe*> recipe_by_name;
            std::unordered_map<std::string, const Item*> item_by_name;
            const void* recipes_ptr = nullptr;
            const void* items_ptr = nullptr;
        };

        LookupCache& Cache()
        {
            static LookupCache c;
            return c;
        }

        void EnsureCacheBuilt()
        {
            LookupCache& c = Cache();
            std::lock_guard<std::mutex> lock(c.mutex);
            const void* current_recipes = static_cast<const void*>(&Data::Recipes());
            const void* current_items = static_cast<const void*>(&Data::Items());
            if (c.recipes_ptr == current_recipes && c.items_ptr == current_items
                && !c.recipe_by_name.empty())
            {
                return;
            }
            c.recipe_by_name.clear();
            c.item_by_name.clear();
            for (const auto& r : Data::Recipes())
            {
                // Recipe::name may start with "*" for alts; match against both forms.
                const std::string& full = r->name;
                c.recipe_by_name[ToLower(full)] = r.get();
                if (!full.empty() && full[0] == '*')
                {
                    c.recipe_by_name[ToLower(full.substr(1))] = r.get();
                }
            }
            for (const auto& [name, item] : Data::Items())
            {
                c.item_by_name[ToLower(name)] = item.get();
            }
            c.recipes_ptr = current_recipes;
            c.items_ptr = current_items;
        }

        const Recipe* LookupRecipe(const std::string& display_name)
        {
            if (display_name.empty())
            {
                return nullptr;
            }
            EnsureCacheBuilt();
            LookupCache& c = Cache();
            std::lock_guard<std::mutex> lock(c.mutex);
            auto it = c.recipe_by_name.find(ToLower(display_name));
            return it == c.recipe_by_name.end() ? nullptr : it->second;
        }

        const Item* LookupItem(const std::string& display_name)
        {
            if (display_name.empty())
            {
                return nullptr;
            }
            EnsureCacheBuilt();
            LookupCache& c = Cache();
            std::lock_guard<std::mutex> lock(c.mutex);
            auto it = c.item_by_name.find(ToLower(display_name));
            return it == c.item_by_name.end() ? nullptr : it->second;
        }

        BuildingKind ParseKind(const std::string& s)
        {
            if (s == "manufacturer")   return BuildingKind::Manufacturer;
            if (s == "miner")          return BuildingKind::Miner;
            if (s == "splitter")       return BuildingKind::Splitter;
            if (s == "smart_splitter") return BuildingKind::SmartSplitter;
            if (s == "prog_splitter")  return BuildingKind::ProgSplitter;
            if (s == "merger")         return BuildingKind::Merger;
            if (s == "sink")           return BuildingKind::Sink;
            if (s == "storage")        return BuildingKind::Storage;
            if (s == "industrial_storage") return BuildingKind::IndustrialStorage;
            if (s == "truck_station")  return BuildingKind::TruckStation;
            if (s == "train_station")  return BuildingKind::TrainStation;
            if (s == "dimensional_depot") return BuildingKind::DimensionalDepot;
            return BuildingKind::Unknown;
        }

        FractionalNumber ClockToFraction(double clock)
        {
            if (!std::isfinite(clock) || clock <= 0.0)
            {
                return FractionalNumber(1, 1);
            }
            // Three decimal places of precision is more than enough; in-game
            // clocks are stored as 6 decimals but visible to the user only at
            // 4 (e.g. 87.5000%).
            const long long num = static_cast<long long>(std::llround(clock * 1000.0));
            return FractionalNumber(num, 1000);
        }

        void ApplyCompactLayout(std::vector<std::unique_ptr<Node>>& nodes, const std::vector<std::unique_ptr<Link>>& links)
        {
            if (nodes.empty())
            {
                return;
            }

            static constexpr float kColumnSpacing = 360.0f;
            static constexpr float kRowSpacing = 170.0f;

            std::unordered_map<const Node*, size_t> node_to_index;
            node_to_index.reserve(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                node_to_index[nodes[i].get()] = i;
            }

            std::vector<std::vector<size_t>> next(nodes.size());
            std::vector<size_t> indegree(nodes.size(), 0);
            for (const auto& link : links)
            {
                if (link == nullptr || link->start == nullptr || link->end == nullptr ||
                    link->start->node == nullptr || link->end->node == nullptr)
                {
                    continue;
                }
                const auto src_it = node_to_index.find(link->start->node);
                const auto dst_it = node_to_index.find(link->end->node);
                if (src_it == node_to_index.end() || dst_it == node_to_index.end())
                {
                    continue;
                }
                next[src_it->second].push_back(dst_it->second);
                indegree[dst_it->second] += 1;
            }

            auto original_order_less = [&](size_t lhs, size_t rhs) {
                if (nodes[lhs]->pos.y != nodes[rhs]->pos.y)
                {
                    return nodes[lhs]->pos.y < nodes[rhs]->pos.y;
                }
                return nodes[lhs]->pos.x < nodes[rhs]->pos.x;
            };

            std::vector<size_t> remaining_indegree = indegree;
            std::vector<int> depth(nodes.size(), 0);
            std::vector<bool> processed(nodes.size(), false);
            std::vector<size_t> ready;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (remaining_indegree[i] == 0)
                {
                    ready.push_back(i);
                }
            }
            std::sort(ready.begin(), ready.end(), original_order_less);

            size_t processed_count = 0;
            while (!ready.empty())
            {
                const size_t current = ready.front();
                ready.erase(ready.begin());
                if (processed[current])
                {
                    continue;
                }
                processed[current] = true;
                processed_count += 1;

                for (size_t dst : next[current])
                {
                    depth[dst] = std::max(depth[dst], depth[current] + 1);
                    if (remaining_indegree[dst] > 0)
                    {
                        remaining_indegree[dst] -= 1;
                    }
                    if (remaining_indegree[dst] == 0 && !processed[dst])
                    {
                        ready.push_back(dst);
                    }
                }
                std::sort(ready.begin(), ready.end(), original_order_less);
            }

            if (processed_count < nodes.size())
            {
                std::vector<size_t> unresolved;
                unresolved.reserve(nodes.size() - processed_count);
                for (size_t i = 0; i < nodes.size(); ++i)
                {
                    if (!processed[i])
                    {
                        unresolved.push_back(i);
                    }
                }
                std::sort(unresolved.begin(), unresolved.end(), [&](size_t lhs, size_t rhs) {
                    if (nodes[lhs]->pos.x != nodes[rhs]->pos.x)
                    {
                        return nodes[lhs]->pos.x < nodes[rhs]->pos.x;
                    }
                    return original_order_less(lhs, rhs);
                });
                for (size_t i = 0; i < unresolved.size(); ++i)
                {
                    depth[unresolved[i]] = static_cast<int>(i / 12);
                }
            }

            int max_depth = 0;
            for (int d : depth)
            {
                max_depth = std::max(max_depth, d);
            }
            std::vector<std::vector<size_t>> columns(static_cast<size_t>(max_depth + 1));
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                columns[static_cast<size_t>(depth[i])].push_back(i);
            }

            for (size_t column = 0; column < columns.size(); ++column)
            {
                auto& column_nodes = columns[column];
                std::sort(column_nodes.begin(), column_nodes.end(), original_order_less);
                const float column_height = static_cast<float>(column_nodes.size() > 0 ? column_nodes.size() - 1 : 0) * kRowSpacing;
                const float y_start = -0.5f * column_height;
                for (size_t row = 0; row < column_nodes.size(); ++row)
                {
                    Node* node = nodes[column_nodes[row]].get();
                    node->pos = ImVec2(
                        static_cast<float>(column) * kColumnSpacing,
                        y_start + static_cast<float>(row) * kRowSpacing
                    );
                }
            }
        }

        // The raw resources an in-game extractor (Miner / Water / Oil / Resource
        // Well) can produce. An extractor can ONLY output one of these, so they
        // are the only valid answers when inferring an extractor's resource from
        // downstream. Mirrors the miner menu in production_app.cpp plus the
        // fluid/gas extractors; kept here (not derived from satisfactory.json)
        // because the data file doesn't tag items as extractable.
        bool IsExtractableResource(const Item* item)
        {
            if (item == nullptr) return false;
            static const std::unordered_set<std::string> kRawResources = {
                "Iron Ore", "Copper Ore", "Caterium Ore", "Limestone", "Coal",
                "Sulfur", "Bauxite", "Raw Quartz", "Uranium", "SAM",
                "Water", "Crude Oil", "Nitrogen Gas",
            };
            return kRawResources.find(item->name) != kRawResources.end();
        }

        // Infer an extractor's resource by following its output belt downstream
        // until a pin carrying a valid raw resource is reached.
        //
        // Only raw extractable resources are accepted as the answer. Organizer
        // and splitter pins can carry stale or filter labels — a smart/
        // programmable splitter's pins show its configured filter item (e.g.
        // "Plastic") even when a different resource physically flows through it,
        // and a miner can only ever produce a mineable resource. Restricting the
        // result to raw resources both rejects those bad labels and makes it
        // safe to walk THROUGH smart/programmable splitters (a splitter's
        // outputs are a subset of its single raw input), so a chain like
        // miner -> smart splitter -> ... still resolves to the real resource.
        //
        // Breadth-first so the nearest raw resource wins if the line later
        // merges with another producer.
        const Item* ResolveExtractorResourceDownstream(Pin* start_pin)
        {
            if (start_pin == nullptr)
            {
                return nullptr;
            }
            std::vector<Pin*> queue;
            std::unordered_set<Node*> visited;
            queue.push_back(start_pin);
            for (size_t head = 0; head < queue.size(); ++head)
            {
                Pin* pin = queue[head];
                if (pin == nullptr) continue;
                if (IsExtractableResource(pin->item))
                {
                    return pin->item;
                }
                Node* node = pin->node;
                if (node == nullptr) continue;
                // Only flow through pass-through nodes; a craft/extractor/sink
                // pin item was already checked above, but we don't traverse
                // their internals.
                if (!node->IsOrganizer() && !node->IsLogistics()) continue;
                if (!visited.insert(node).second) continue;
                for (const auto& p : node->ins)
                {
                    if (IsExtractableResource(p->item)) return p->item;
                }
                for (const auto& p : node->outs)
                {
                    if (IsExtractableResource(p->item)) return p->item;
                    if (p->link != nullptr && p->link->end != nullptr)
                    {
                        queue.push_back(p->link->end);
                    }
                }
            }
            return nullptr;
        }

        void ResolveExtractorResources(std::vector<std::unique_ptr<Node>>& nodes,
            const std::function<unsigned long long int()>& id_generator)
        {
            for (const auto& node : nodes)
            {
                if (!node->IsExtractor() || node->outs.empty() || node->outs[0]->link == nullptr)
                {
                    continue;
                }
                ExtractorNode* extractor = static_cast<ExtractorNode*>(node.get());
                if (extractor->resource != nullptr)
                {
                    continue;
                }
                const Item* item = ResolveExtractorResourceDownstream(node->outs[0]->link->end);
                if (item != nullptr)
                {
                    extractor->ChangeResource(item, id_generator);
                }
            }
        }
    }

    ParseResult ParseWrapperJson(const std::string& json)
    {
        ParseResult result;
        Json::Value root;
        try
        {
            root = Json::Parse(json);
        }
        catch (const std::exception& e)
        {
            result.error = std::string("Failed to parse wrapper JSON: ") + e.what();
            return result;
        }

        if (root.is_null() || !root.is_object())
        {
            result.error = "Wrapper JSON has no root object";
            return result;
        }

        if (root.contains("buildings") && root["buildings"].is_array())
        {
            for (const auto& b : root["buildings"].get_array())
            {
                Building building;
                if (b.contains("id") && b["id"].is_string())
                {
                    building.id = b["id"].get_string();
                }
                if (b.contains("kind") && b["kind"].is_string())
                {
                    building.kind = ParseKind(b["kind"].get_string());
                }
                if (b.contains("recipe_name") && b["recipe_name"].is_string())
                {
                    building.recipe_name = b["recipe_name"].get_string();
                }
                if (b.contains("item_name") && b["item_name"].is_string())
                {
                    building.item_name = b["item_name"].get_string();
                }
                if (b.contains("fuel_item") && b["fuel_item"].is_string())
                {
                    building.fuel_item = b["fuel_item"].get_string();
                }
                if (b.contains("cargo_item") && b["cargo_item"].is_string())
                {
                    building.cargo_item = b["cargo_item"].get_string();
                }
                if (b.contains("is_unloader") && b["is_unloader"].is_bool())
                {
                    building.is_unloader = b["is_unloader"].get<bool>();
                }
                if (b.contains("clock") && b["clock"].is_number())
                {
                    building.clock = b["clock"].get<double>();
                }
                if (b.contains("somersloops") && b["somersloops"].is_number())
                {
                    building.somersloops = b["somersloops"].get<int>();
                }
                if (b.contains("extractor_kind") && b["extractor_kind"].is_number())
                {
                    building.extractor_kind = b["extractor_kind"].get<int>();
                }
                if (b.contains("extractor_purity") && b["extractor_purity"].is_string())
                {
                    building.extractor_purity = b["extractor_purity"].get_string();
                }
                if (b.contains("pos") && b["pos"].is_array())
                {
                    const auto& arr = b["pos"].get_array();
                    if (arr.size() >= 1 && arr[0].is_number()) building.x = arr[0].get<double>();
                    if (arr.size() >= 2 && arr[1].is_number()) building.y = arr[1].get<double>();
                }
                auto read_ports = [](const Json::Value& v, std::vector<std::string>& out_ports)
                {
                    if (!v.is_array()) return;
                    for (const auto& port : v.get_array())
                    {
                        std::string belt_id;
                        if (port.is_object() && port.contains("belt_id") && port["belt_id"].is_string())
                        {
                            belt_id = port["belt_id"].get_string();
                        }
                        out_ports.push_back(belt_id);
                    }
                };
                if (b.contains("inputs")) read_ports(b["inputs"], building.input_belt_ids);
                if (b.contains("outputs")) read_ports(b["outputs"], building.output_belt_ids);
                building.input_count = static_cast<int>(building.input_belt_ids.size());
                building.output_count = static_cast<int>(building.output_belt_ids.size());
                result.buildings.push_back(std::move(building));
            }
        }

        if (root.contains("belts") && root["belts"].is_array())
        {
            for (const auto& bl : root["belts"].get_array())
            {
                Belt belt;
                if (bl.contains("id") && bl["id"].is_string()) belt.id = bl["id"].get_string();
                if (bl.contains("item_name") && bl["item_name"].is_string()) belt.item_name = bl["item_name"].get_string();
                if (bl.contains("src") && bl["src"].is_object())
                {
                    if (bl["src"].contains("building") && bl["src"]["building"].is_string())
                    {
                        belt.src_building = bl["src"]["building"].get_string();
                    }
                    if (bl["src"].contains("port") && bl["src"]["port"].is_number())
                    {
                        belt.src_port = bl["src"]["port"].get<int>();
                    }
                    if (bl["src"].contains("role") && bl["src"]["role"].is_string())
                    {
                        belt.src_role = bl["src"]["role"].get_string();
                    }
                    if (bl["src"].contains("dir") && bl["src"]["dir"].is_string())
                    {
                        belt.src_dir = bl["src"]["dir"].get_string();
                    }
                }
                if (bl.contains("dst") && bl["dst"].is_object())
                {
                    if (bl["dst"].contains("building") && bl["dst"]["building"].is_string())
                    {
                        belt.dst_building = bl["dst"]["building"].get_string();
                    }
                    if (bl["dst"].contains("port") && bl["dst"]["port"].is_number())
                    {
                        belt.dst_port = bl["dst"]["port"].get<int>();
                    }
                    if (bl["dst"].contains("role") && bl["dst"]["role"].is_string())
                    {
                        belt.dst_role = bl["dst"]["role"].get_string();
                    }
                    if (bl["dst"].contains("dir") && bl["dst"]["dir"].is_string())
                    {
                        belt.dst_dir = bl["dst"]["dir"].get_string();
                    }
                }
                result.belts.push_back(std::move(belt));
            }
        }

        if (root.contains("warnings") && root["warnings"].is_array())
        {
            for (const auto& w : root["warnings"].get_array())
            {
                if (w.is_string()) result.warnings.push_back(w.get_string());
            }
        }

        // Logistics block (vehicle routes + stations). Additive: consumed only
        // when BuildOptions::connect_vehicle_routes is set. Routes are resolved
        // from their GUIDs to the matching station ids here.
        if (root.contains("logistics") && root["logistics"].is_object())
        {
            const Json::Value& L = root["logistics"];
            std::unordered_map<std::string, std::string> guid_to_station;
            if (L.contains("stations") && L["stations"].is_array())
            {
                for (const auto& s : L["stations"].get_array())
                {
                    LogisticsStation st;
                    if (s.contains("id") && s["id"].is_string()) st.id = s["id"].get_string();
                    if (s.contains("guid") && s["guid"].is_string()) st.guid = s["guid"].get_string();
                    if (st.id.empty()) continue;
                    if (!st.guid.empty()) guid_to_station[st.guid] = st.id;
                    result.logistics_stations.push_back(std::move(st));
                }
            }
            if (L.contains("vehicles") && L["vehicles"].is_array())
            {
                for (const auto& v : L["vehicles"].get_array())
                {
                    if (!v.contains("route_guids") || !v["route_guids"].is_array()) continue;
                    std::vector<std::string> route;
                    for (const auto& g : v["route_guids"].get_array())
                    {
                        if (!g.is_string()) continue;
                        auto it = guid_to_station.find(g.get_string());
                        if (it != guid_to_station.end()) route.push_back(it->second);
                    }
                    if (route.size() >= 2) result.vehicle_routes.push_back(std::move(route));
                }
            }
        }

        result.ok = true;
        return result;
    }

    bool BuildGraph(const ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        BuildOutput& out,
        std::string& err,
        const BuildOptions& options)
    {
        if (!parsed.ok)
        {
            err = parsed.error.empty() ? "Parse failed" : parsed.error;
            return false;
        }

        out.warnings = parsed.warnings;

        // Map from wrapper building id → index into out.nodes
        std::unordered_map<std::string, size_t> id_to_index;
        id_to_index.reserve(parsed.buildings.size());

        // Save-port indices in .sav are not a uniform 0..N-1 sequence: splitters
        // and mergers use bare 1-based names (Input1, Output1..3), while
        // manufacturers and storage expose conveyor connections with
        // descriptive names that produce save indices like -1 or arbitrary
        // small integers (e.g. smelter conveyor at save-port 1 even though the
        // CraftNode has a single output pin). To turn that into something the
        // node pin layout can absorb, gather the distinct save-ports seen on
        // each (building, direction) and assign C++ pin indices densely in
        // sorted save-port order. Splitters/mergers, which already use 0/1/2,
        // get an identity mapping; the manufacturers / storage / truck cases
        // collapse onto pin 0 (and onto pin 1, etc. when a building has
        // multiple distinct save-ports in that direction).
        //
        // Belts flagged role:"fuel" by the wrapper land on a dedicated fuel
        // pin (last input on stations) and are excluded from the cargo rank
        // set so they don't shift the cargo pin assignment.
        // Conveyor belts are one-way: they leave a producer's output and arrive
        // at a consumer's input. The wrapper preserves each endpoint's direction
        // ("out"/"in"/"any"; empty for legacy output that predates this field).
        // An edge whose source is an input or whose destination is an output is
        // physically impossible and, if wired, lands on the wrong side of a
        // building and collides with its real belt — so we reject it here.
        // Unknown ("" / "any") directions are allowed, preserving old behaviour.
        auto belt_violates_one_way = [](const Belt& belt) {
            return belt.src_dir == "in" || belt.dst_dir == "out";
        };

        // ---- Station fuel-belt classification ----
        // A Truck/Train station has a dedicated fuel inlet separate from its
        // cargo conveyor, but the save marks neither the connector nor the belt
        // as "fuel": the fuel inlet is a blueprint detail the save omits, and the
        // connector names are aliased, so neither connector identity nor item
        // type can tell fuel from cargo (a station may haul Coal as cargo and
        // burn Coal as fuel, or haul Coal and burn Turbofuel — both fuels). What
        // the save DOES record is each station's FuelInventory and cargo
        // inventory contents, exported here as Building::fuel_item / cargo_item.
        // We tag the belts feeding a station's fuel inlet with role:"fuel" using,
        // in order: the station's own fuel item, the factory-wide set of fuel
        // items, and (when the inventory is empty) the unload-mode rule that an
        // unloader's input belt can only be fuel. This is done on a local copy of
        // the belts so the caller's ParseResult stays const. Tagged belts land on
        // the dedicated fuel pin and never bleed into the cargo item-space.
        std::vector<Belt> belts = parsed.belts;

        std::unordered_map<std::string, const Building*> building_by_id;
        building_by_id.reserve(parsed.buildings.size());
        for (const Building& b : parsed.buildings) building_by_id[b.id] = &b;

        std::unordered_set<std::string> factory_fuel_items;
        for (const Building& b : parsed.buildings)
        {
            if (!b.fuel_item.empty()) factory_fuel_items.insert(b.fuel_item);
        }

        std::unordered_map<std::string, std::vector<size_t>> incoming_belts_of;
        std::unordered_map<std::string, std::vector<size_t>> outgoing_belts_of;
        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            if (belt_violates_one_way(belts[bi])) continue;
            incoming_belts_of[belts[bi].dst_building].push_back(bi);
            outgoing_belts_of[belts[bi].src_building].push_back(bi);
        }

        // A miner the save left without a resource (item_name empty) is resolved
        // by ResolveExtractorResources at the END of this function, on the wired
        // graph — too late for the fuel classification below, which runs before
        // wiring. So resolve it here on the parsed belt graph the same way: walk
        // DOWNSTREAM to the first raw extractable resource a craft consumes. This
        // lets a fuel line whose source miner has a blank resource still be
        // matched by item (not just caught by the unloader fallback).
        auto resolve_miner_resource = [&](const std::string& miner_id) -> std::string {
            std::unordered_set<std::string> visited;
            std::vector<std::string> queue;
            if (auto it = outgoing_belts_of.find(miner_id); it != outgoing_belts_of.end())
            {
                for (size_t bi : it->second) queue.push_back(belts[bi].dst_building);
            }
            for (size_t head = 0; head < queue.size(); ++head) // BFS: nearest craft wins
            {
                const std::string cur = queue[head];
                if (!visited.insert(cur).second) continue;
                auto bit = building_by_id.find(cur);
                if (bit == building_by_id.end()) continue;
                const Building* b = bit->second;
                if (b->kind == BuildingKind::Manufacturer)
                {
                    const Recipe* r = LookupRecipe(b->recipe_name);
                    if (r != nullptr)
                    {
                        for (const auto& in : r->ins)
                        {
                            if (in.item != nullptr && IsExtractableResource(in.item)) return in.item->name;
                        }
                    }
                    continue; // craft with no raw input: don't trace past it
                }
                if (b->kind == BuildingKind::Miner ||
                    b->kind == BuildingKind::TruckStation ||
                    b->kind == BuildingKind::TrainStation)
                {
                    continue; // another producer / vehicle break: stop this branch
                }
                if (auto oit = outgoing_belts_of.find(cur); oit != outgoing_belts_of.end())
                {
                    for (size_t bi : oit->second) queue.push_back(belts[bi].dst_building);
                }
            }
            return std::string();
        };

        std::unordered_map<std::string, std::string> resolved_miner_resource;
        for (const Building& b : parsed.buildings)
        {
            if (b.kind == BuildingKind::Miner && b.item_name.empty())
            {
                std::string r = resolve_miner_resource(b.id);
                if (!r.empty()) resolved_miner_resource[b.id] = r;
            }
        }

        // Walk upstream from a belt's source through pass-through buildings
        // (splitters/mergers/storage — including smart splitters, whose outputs
        // are a subset of their single input) to the nearest producer, returning
        // the item it makes. Stops at stations: a vehicle decouples a station's
        // two sides, so tracing through one is meaningless.
        auto trace_source_item = [&](const std::string& start_building) -> std::string {
            std::unordered_set<std::string> visited;
            std::vector<std::string> stack{ start_building };
            while (!stack.empty())
            {
                const std::string cur = stack.back();
                stack.pop_back();
                if (!visited.insert(cur).second) continue;
                auto it = building_by_id.find(cur);
                if (it == building_by_id.end()) continue;
                const Building* b = it->second;
                if (b->kind == BuildingKind::Miner)
                {
                    if (!b->item_name.empty()) return b->item_name;
                    auto rit = resolved_miner_resource.find(cur);
                    return rit != resolved_miner_resource.end() ? rit->second : std::string();
                }
                if (b->kind == BuildingKind::Manufacturer)
                {
                    const Recipe* r = LookupRecipe(b->recipe_name);
                    if (r != nullptr && r->outs.size() == 1 && r->outs[0].item != nullptr)
                    {
                        return r->outs[0].item->name;
                    }
                    continue; // multi-output: can't attribute this belt's item
                }
                if (b->kind == BuildingKind::TruckStation || b->kind == BuildingKind::TrainStation)
                {
                    continue; // vehicle break
                }
                auto in_it = incoming_belts_of.find(cur);
                if (in_it != incoming_belts_of.end())
                {
                    for (size_t oi : in_it->second) stack.push_back(belts[oi].src_building);
                }
            }
            return std::string();
        };

        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            Belt& belt = belts[bi];
            if (belt_violates_one_way(belt)) continue;
            if (belt.dst_role == "fuel") continue; // already tagged upstream
            auto it = building_by_id.find(belt.dst_building);
            if (it == building_by_id.end()) continue;
            const Building* station = it->second;
            if (station->kind != BuildingKind::TruckStation &&
                station->kind != BuildingKind::TrainStation) continue;

            const std::string item = trace_source_item(belt.src_building);
            const bool item_is_fuel = !item.empty() &&
                ((!station->fuel_item.empty() && item == station->fuel_item)
                 || factory_fuel_items.count(item) > 0);
            bool is_fuel = false;
            if (station->is_unloader)
            {
                // An unloader receives its cargo by VEHICLE and emits it on the
                // OUTPUT conveyor, so a belt on an INPUT connector is the fuel
                // inlet — even when the fuel is the same item the station ships
                // (e.g. Coal cargo arriving by truck + Coal fuel arriving by
                // belt). Accept it as fuel when the item is a known fuel here or
                // is unresolved; a clearly-non-fuel item is left as cargo (likely
                // a mis-read side) and, since the fuel-pin wiring re-checks
                // IsFuelItem, a non-fuel never lands on the fuel pin anyway.
                if (item.empty() || item_is_fuel) is_fuel = true;
            }
            else if (item_is_fuel)
            {
                // Loader: cargo arrives on a belt input and fuel is the extra
                // input. Distinguish by item — the cargo input matches the cargo
                // inventory, so only a non-cargo fuel item is the fuel belt.
                const bool is_cargo = !station->cargo_item.empty() && item == station->cargo_item;
                if (!is_cargo) is_fuel = true;
            }
            if (is_fuel) belt.dst_role = "fuel";
        }

        std::unordered_map<std::string, std::set<int>> observed_in_ports;
        std::unordered_map<std::string, std::set<int>> observed_out_ports;
        size_t wrong_side_belts = 0;
        for (const Belt& belt : belts)
        {
            if (belt_violates_one_way(belt)) { wrong_side_belts += 1; continue; }
            if (belt.src_role != "fuel")
            {
                observed_out_ports[belt.src_building].insert(belt.src_port);
            }
            if (belt.dst_role != "fuel")
            {
                observed_in_ports[belt.dst_building].insert(belt.dst_port);
            }
        }

        for (const Building& b : parsed.buildings)
        {
            std::unique_ptr<Node> node;
            switch (b.kind)
            {
            case BuildingKind::Manufacturer:
            {
                const Recipe* recipe = LookupRecipe(b.recipe_name);
                if (recipe == nullptr)
                {
                    if (b.recipe_name.empty())
                    {
                        out.warnings.push_back("Skipping " + b.id + ": no recipe set");
                    }
                    else
                    {
                        out.warnings.push_back("Skipping " + b.id + ": unmapped recipe '" + b.recipe_name + "'");
                    }
                    continue;
                }
                auto craft = std::make_unique<CraftNode>(id_generator(), recipe, id_generator);
                craft->num_somersloop = FractionalNumber(static_cast<long long>(b.somersloops));
                craft->UpdateRate(ClockToFraction(b.clock));
                node = std::move(craft);
                break;
            }
            case BuildingKind::Miner:
            {
                // Map the wrapper's extractor_kind enum onto ExtractorNode::Kind.
                // 1/2/3 = Miner Mk1/2/3, 4 = Water Extractor, 5 = Oil Extractor.
                // 0 (unset) falls back to MinerMk1 so we still place a node.
                ExtractorNode::Kind ekind = ExtractorNode::Kind::MinerMk1;
                switch (b.extractor_kind)
                {
                case 1: ekind = ExtractorNode::Kind::MinerMk1; break;
                case 2: ekind = ExtractorNode::Kind::MinerMk2; break;
                case 3: ekind = ExtractorNode::Kind::MinerMk3; break;
                case 4: ekind = ExtractorNode::Kind::WaterExtractor; break;
                case 5: ekind = ExtractorNode::Kind::OilExtractor; break;
                default: break;
                }

                ExtractorNode::Purity purity = ExtractorNode::Purity::Normal;
                if (b.extractor_purity == "impure")    purity = ExtractorNode::Purity::Impure;
                else if (b.extractor_purity == "pure") purity = ExtractorNode::Purity::Pure;

                const Item* resource = LookupItem(b.item_name);
                if (resource == nullptr && !b.item_name.empty())
                {
                    out.warnings.push_back("Extractor " + b.id + ": unmapped resource '" + b.item_name + "' (placing without resource)");
                }

                auto extractor = std::make_unique<ExtractorNode>(id_generator(), ekind, resource, purity, id_generator);
                extractor->UpdateRate(ClockToFraction(b.clock));
                node = std::move(extractor);
                break;
            }
            case BuildingKind::Splitter:
            {
                node = std::make_unique<GameSplitterNode>(id_generator(), id_generator, LookupItem(b.item_name));
                break;
            }
            case BuildingKind::SmartSplitter:
            case BuildingKind::ProgSplitter:
            {
                node = std::make_unique<CustomSplitterNode>(id_generator(), id_generator, LookupItem(b.item_name));
                break;
            }
            case BuildingKind::Merger:
            {
                node = std::make_unique<MergerNode>(id_generator(), id_generator, LookupItem(b.item_name));
                break;
            }
            case BuildingKind::Sink:
            {
                node = std::make_unique<SinkNode>(id_generator(), id_generator, LookupItem(b.item_name));
                break;
            }
            case BuildingKind::Storage:
            case BuildingKind::IndustrialStorage:
            case BuildingKind::TruckStation:
            case BuildingKind::TrainStation:
            case BuildingKind::DimensionalDepot:
            {
                LogisticsNode::Kind kind = LogisticsNode::Kind::Storage;
                int default_cargo_inputs = 1;
                int default_cargo_outputs = 1;
                // Truck/Train stations expose a dedicated fuel inlet on top of
                // their cargo belts. We always allocate the fuel pin so the
                // editor shows the slot even when the player hasn't piped fuel
                // in yet; fuel-role belts get routed to it explicitly below.
                bool has_fuel_input_pin = false;
                if (b.kind == BuildingKind::IndustrialStorage)
                {
                    kind = LogisticsNode::Kind::IndustrialStorage;
                    default_cargo_inputs = 2;
                    default_cargo_outputs = 2;
                }
                else if (b.kind == BuildingKind::TruckStation || b.kind == BuildingKind::TrainStation)
                {
                    kind = b.kind == BuildingKind::TruckStation
                        ? LogisticsNode::Kind::TruckStation
                        : LogisticsNode::Kind::TrainStation;
                    // Stations physically expose several cargo connectors (the
                    // blueprint pattern wires belts to all of them and leaves the
                    // unused ones dead-ending). Allocating a fixed 2 in / 2 out
                    // left those unused connectors as empty "dead-end" pins on the
                    // node. Instead allocate purely from the connectors that carry
                    // a real belt (observed ports below); the LogisticsNode floor
                    // keeps a single cargo pin per side so the node stays usable,
                    // and the vehicle-route step grows a pin if it needs one.
                    default_cargo_inputs = 0;
                    default_cargo_outputs = 0;
                    has_fuel_input_pin = true;
                }
                else if (b.kind == BuildingKind::DimensionalDepot)
                {
                    // Dimensional Depot Uploader has a single belt input and no
                    // belt output — items flow into the shared dimensional
                    // inventory rather than down a conveyor.
                    kind = LogisticsNode::Kind::DimensionalDepot;
                    default_cargo_inputs = 1;
                    default_cargo_outputs = 0;
                }
                const int cargo_in = static_cast<int>(observed_in_ports[b.id].size());
                const int cargo_out = static_cast<int>(observed_out_ports[b.id].size());
                const size_t total_in = static_cast<size_t>(std::max(default_cargo_inputs, cargo_in))
                    + (has_fuel_input_pin ? 1u : 0u);
                const size_t total_out = static_cast<size_t>(std::max(default_cargo_outputs, cargo_out));
                node = std::make_unique<LogisticsNode>(id_generator(), kind,
                    total_in, total_out, id_generator);
                break;
            }
            case BuildingKind::Unknown:
            default:
                out.warnings.push_back("Skipping building with unknown kind: " + b.id);
                continue;
            }

            const float world_scale = options.world_spacing_scale > 0.0f ? options.world_spacing_scale : kPositionScale;
            node->pos = ImVec2(b.x * world_scale, b.y * world_scale);
            id_to_index[b.id] = out.nodes.size();
            out.nodes.push_back(std::move(node));
        }

        // Wire belts → Links. Aggregate per-belt failures into single warning
        // lines so a save with hundreds of marginal belts doesn't spam.
        size_t port_out_of_range = 0;
        size_t collisions = 0;
        size_t connected = 0;
        // Rank of a save-port within its building's sorted set of distinct
        // save-ports for that direction → C++ pin index.
        auto save_port_rank = [](const std::set<int>& ports, int save_port) -> int {
            int rank = 0;
            for (int p : ports)
            {
                if (p == save_port) return rank;
                rank += 1;
            }
            return -1;
        };

        // Resolve the item flowing on each belt. The save records save-port
        // indices that get mapped densely onto C++ pin indices, but for
        // multi-input/output machines (e.g. an Assembler with Iron Plate +
        // Screw, or a Refinery with two outputs) the rank-based mapping can
        // route a belt to the wrong recipe slot, leaving crossed items and
        // mismatched rates. Knowing the item per belt lets us route by
        // item-match on CraftNode endpoints instead.
        std::unordered_map<std::string, std::vector<size_t>> belts_by_src_building;
        std::unordered_map<std::string, std::vector<size_t>> belts_by_dst_building;
        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            if (belt_violates_one_way(belts[bi])) continue;
            belts_by_src_building[belts[bi].src_building].push_back(bi);
            belts_by_dst_building[belts[bi].dst_building].push_back(bi);
        }
        std::vector<const Item*> belt_item(belts.size(), nullptr);
        // Seed phase: any belt whose endpoint is a single-output extractor or
        // single-output/single-input craft has an unambiguous item.
        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            const Belt& belt = belts[bi];
            if (belt_violates_one_way(belt)) continue;
            if (auto src_it = id_to_index.find(belt.src_building); src_it != id_to_index.end())
            {
                Node* node = out.nodes[src_it->second].get();
                if (node->IsExtractor())
                {
                    belt_item[bi] = static_cast<const ExtractorNode*>(node)->resource;
                }
                else if (node->IsCraft())
                {
                    const CraftNode* c = static_cast<const CraftNode*>(node);
                    if (c->recipe != nullptr && c->recipe->outs.size() == 1)
                    {
                        belt_item[bi] = c->recipe->outs[0].item;
                    }
                    else if (c->recipe != nullptr && c->recipe->outs.size() > 1)
                    {
                        // Multi-output machines (Refinery/Blender/...): the belt
                        // leaves one specific output port. Map it to a recipe
                        // output by the same save-port rank the wiring uses, so a
                        // Plastic refinery's Plastic belt is typed Plastic up front
                        // instead of being left unresolved and then back-filled
                        // with a downstream consumer's ingredient. CraftNode output
                        // pins are built in recipe->outs order, so rank lines up.
                        const int rank = save_port_rank(observed_out_ports[belt.src_building], belt.src_port);
                        if (rank >= 0 && static_cast<size_t>(rank) < c->recipe->outs.size())
                        {
                            belt_item[bi] = c->recipe->outs[rank].item;
                        }
                    }
                }
            }
            if (belt_item[bi] != nullptr) continue;
            if (auto dst_it = id_to_index.find(belt.dst_building); dst_it != id_to_index.end())
            {
                Node* node = out.nodes[dst_it->second].get();
                if (node->IsCraft())
                {
                    const CraftNode* c = static_cast<const CraftNode*>(node);
                    if (c->recipe != nullptr && c->recipe->ins.size() == 1)
                    {
                        belt_item[bi] = c->recipe->ins[0].item;
                    }
                }
            }
        }
        // Propagate through organizers and logistics: a game splitter, merger,
        // or storage forwards a single item, so if any belt touching it has a
        // known item, every belt touching it carries the same item.
        //
        // Smart / programmable splitters are deliberately excluded: their
        // three outputs can carry different items (that's the entire point of
        // smart filtering), and treating them as item-uniform causes a chain
        // like coal-miner → smart-splitter → {production, overflow} to back-
        // propagate the wrong item from one branch onto the miner and the
        // other branches.
        const size_t max_iters = belts.size() + 8;
        for (size_t iter = 0; iter < max_iters; ++iter)
        {
            bool changed = false;
            for (size_t bi = 0; bi < belts.size(); ++bi)
            {
                if (belt_item[bi] != nullptr) continue;
                const Belt& belt = belts[bi];
                if (belt_violates_one_way(belt)) continue;
                const Item* found = nullptr;
                // Fuel belts form their own item space, separate from cargo: a
                // fuel belt may only inherit from another fuel belt and a cargo
                // belt only from cargo belts. This stops a station's fuel item
                // from bleeding onto its cargo belts (and vice versa).
                const bool bi_is_fuel = IsFuelBelt(belt);
                auto inherit_from_organizer = [&](const std::string& building_id) {
                    if (found != nullptr) return;
                    auto idx = id_to_index.find(building_id);
                    if (idx == id_to_index.end()) return;
                    Node* node = out.nodes[idx->second].get();
                    if (!node->IsOrganizer() && !node->IsLogistics()) return;
                    if (node->IsCustomSplitter())
                    {
                        // A splitter's outputs are always a subset of its
                        // inputs, so a smart/programmable splitter fed by a
                        // single distinct item can only emit that item on every
                        // output. Propagate that one input item onto this
                        // splitter's OUTPUT belts only — never onto its inputs
                        // from an output, nor between sibling outputs. That lets
                        // a known upstream resource (e.g. a coal miner) reach the
                        // plain organizers and crafts downstream of the splitter,
                        // while a splitter genuinely fed by two different items
                        // stays a propagation wall (its branches may be filtered
                        // apart). Stay conservative: bail if any input is still
                        // unresolved so a later-discovered second item can't have
                        // already mislabeled an output.
                        if (building_id != belt.src_building) return;
                        const Item* single_in = nullptr;
                        for (size_t oi : belts_by_dst_building[building_id])
                        {
                            if (IsFuelBelt(belts[oi]) != bi_is_fuel) continue;
                            if (belt_item[oi] == nullptr) return;
                            if (single_in == nullptr) single_in = belt_item[oi];
                            else if (single_in != belt_item[oi]) return;
                        }
                        found = single_in;
                        return;
                    }
                    auto try_sibling = [&](size_t oi) -> bool {
                        if (oi == bi || belt_item[oi] == nullptr) return false;
                        if (IsFuelBelt(belts[oi]) != bi_is_fuel) return false;
                        found = belt_item[oi];
                        return true;
                    };
                    // Input has priority for deciding a storage's material: a belt
                    // entering the building (dst) reflects what it actually holds,
                    // whereas an outgoing belt (src) may have been tagged by a
                    // single-input downstream machine. Check inputs first so a
                    // downstream item (e.g. a Circuit Board manufacturer) never
                    // contaminates a storage chain; outputs are only a fallback.
                    for (size_t oi : belts_by_dst_building[building_id])
                    {
                        if (try_sibling(oi)) return;
                    }
                    for (size_t oi : belts_by_src_building[building_id])
                    {
                        if (try_sibling(oi)) return;
                    }
                };
                inherit_from_organizer(belt.src_building);
                inherit_from_organizer(belt.dst_building);
                if (found != nullptr)
                {
                    belt_item[bi] = found;
                    changed = true;
                }
            }
            if (!changed) break;
        }

        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            const Belt& belt = belts[bi];
            // One-way guard: never wire a belt whose source is an input or whose
            // destination is an output — that would attach a building's wrong
            // side and collide with its legitimate belt.
            if (belt_violates_one_way(belt)) continue;
            auto src_it = id_to_index.find(belt.src_building);
            auto dst_it = id_to_index.find(belt.dst_building);
            if (src_it == id_to_index.end() || dst_it == id_to_index.end())
            {
                continue;
            }
            Node* src_node = out.nodes[src_it->second].get();
            Node* dst_node = out.nodes[dst_it->second].get();

            int src_pin = -1;
            int dst_pin = -1;
            // Fuel inlets on Truck/Train stations live on a pin that the C++
            // node reserves at the end of its input/output list; route those
            // belts straight to it so the rank-based cargo mapping below never
            // claims the fuel pin for an item conveyor.
            //
            // We additionally require that the belt's resolved item is a
            // recognized fuel (or unresolved). The wrapper's fuel detection
            // is a name-substring match and occasionally fires on a
            // non-fuel connection component; without this guard, a Circuit
            // Board belt that happens to land on a fuel-named port gets
            // routed to the fuel pin and visually breaks the station.
            const bool belt_item_is_fuel_or_unknown =
                belt_item[bi] == nullptr || IsFuelItem(belt_item[bi]);
            if (belt.dst_role == "fuel" && belt_item_is_fuel_or_unknown
                && dst_node->IsLogistics() && !dst_node->ins.empty())
            {
                LogisticsNode* l = static_cast<LogisticsNode*>(dst_node);
                if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                    l->logistics_kind == LogisticsNode::Kind::TrainStation)
                {
                    dst_pin = static_cast<int>(dst_node->ins.size() - 1);
                }
            }
            if (belt.src_role == "fuel" && belt_item_is_fuel_or_unknown
                && src_node->IsLogistics() && !src_node->outs.empty())
            {
                LogisticsNode* l = static_cast<LogisticsNode*>(src_node);
                if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                    l->logistics_kind == LogisticsNode::Kind::TrainStation)
                {
                    src_pin = static_cast<int>(src_node->outs.size() - 1);
                }
            }
            // Prefer item-aware routing for multi-port CraftNodes — fixes the
            // case where a Reinforced Iron Plate assembler has Iron Plate going
            // into the Screw pin because the save's physical port order does
            // not match the recipe's ingredient declaration order.
            if (belt_item[bi] != nullptr)
            {
                if (src_node->IsCraft() && src_node->outs.size() > 1)
                {
                    for (size_t i = 0; i < src_node->outs.size(); ++i)
                    {
                        if (src_node->outs[i]->item == belt_item[bi] && src_node->outs[i]->link == nullptr)
                        {
                            src_pin = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (dst_node->IsCraft() && dst_node->ins.size() > 1)
                {
                    for (size_t i = 0; i < dst_node->ins.size(); ++i)
                    {
                        if (dst_node->ins[i]->item == belt_item[bi] && dst_node->ins[i]->link == nullptr)
                        {
                            dst_pin = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            if (src_pin < 0)
            {
                src_pin = save_port_rank(observed_out_ports[belt.src_building], belt.src_port);
            }
            if (dst_pin < 0)
            {
                dst_pin = save_port_rank(observed_in_ports[belt.dst_building], belt.dst_port);
            }

            if (src_pin < 0 || static_cast<size_t>(src_pin) >= src_node->outs.size() ||
                dst_pin < 0 || static_cast<size_t>(dst_pin) >= dst_node->ins.size())
            {
                port_out_of_range += 1;
                continue;
            }

            Pin* out_pin = src_node->outs[src_pin].get();
            Pin* in_pin = dst_node->ins[dst_pin].get();

            if (in_pin->node->IsLogistics())
            {
                in_pin->item = out_pin->item;
                in_pin->current_rate = out_pin->current_rate;
            }
            if (out_pin->node->IsLogistics())
            {
                // Only mirror the rate. Mirroring the item from a downstream
                // consumer back onto a logistics output is wrong: a Coal
                // storage whose output goes to a Circuit Board manufacturer
                // would inherit "Circuit Board" as its output item even
                // though it physically contains coal. Items on logistics
                // outputs are propagated forward from their inputs in the
                // rate-propagation pass further down.
                out_pin->current_rate = in_pin->current_rate;
            }

            // Skip duplicate connections (a pin can hold at most one link).
            if (out_pin->link != nullptr || in_pin->link != nullptr)
            {
                collisions += 1;
                continue;
            }

            out.links.emplace_back(std::make_unique<Link>(id_generator(), out_pin, in_pin));
            out_pin->link = out.links.back().get();
            in_pin->link = out.links.back().get();
            connected += 1;
        }

        // Plain mergers/splitters and logistics nodes are modeled as carrying
        // one item stream. If the save physically mixes different cargo items
        // through one of those pass-through nodes, keep it untyped and warn
        // instead of silently painting the whole chain as whichever item was
        // seen first. This runs after wiring so multi-output machines (e.g.
        // refineries) contribute the actual connected pin item.
        std::unordered_set<Node*> mixed_item_nodes;
        for (const auto& [building_id, node_index] : id_to_index)
        {
            Node* node = out.nodes[node_index].get();
            if ((!node->IsOrganizer() && !node->IsLogistics()) || node->IsCustomSplitter())
            {
                continue;
            }

            std::set<std::string> cargo_items;
            for (const auto& pin : node->ins)
            {
                if (IsStationFuelPin(node, pin.get())) continue;
                if (pin->link != nullptr && pin->link->start != nullptr
                    && pin->link->start->item != nullptr)
                {
                    cargo_items.insert(pin->link->start->item->name);
                }
            }

            if (cargo_items.size() <= 1) continue;

            mixed_item_nodes.insert(node);
            std::string items;
            for (const std::string& item : cargo_items)
            {
                if (!items.empty()) items += ", ";
                items += item;
            }
            out.warnings.push_back("mixed item pass-through at " + building_id + ": " + items);
        }

        // Once belts are wired and items are resolved, push items into the
        // organizer/logistics pins so the editor renders icons on each pin and
        // future rate edits propagate without item-mismatch rejections.
        //
        // CustomSplitter (smart/programmable) is skipped: OrganizerNode::
        // ChangeItem sets every pin's item, which would force all three
        // outputs to one item even if each output is meant to carry a
        // different item per the in-game filter rules. We instead let the
        // per-pin item assignment happen naturally when each output's belt
        // connects to a typed downstream node.
        for (size_t bi = 0; bi < belts.size(); ++bi)
        {
            if (belt_item[bi] == nullptr) continue;
            const Belt& belt = belts[bi];
            if (belt_violates_one_way(belt)) continue;
            if (auto it = id_to_index.find(belt.src_building); it != id_to_index.end())
            {
                Node* node = out.nodes[it->second].get();
                if (node->IsOrganizer() && !node->IsCustomSplitter())
                {
                    if (mixed_item_nodes.find(node) != mixed_item_nodes.end()) continue;
                    OrganizerNode* org = static_cast<OrganizerNode*>(node);
                    if (org->item == nullptr) org->ChangeItem(belt_item[bi]);
                }
            }
            if (auto it = id_to_index.find(belt.dst_building); it != id_to_index.end())
            {
                Node* node = out.nodes[it->second].get();
                if (node->IsOrganizer() && !node->IsCustomSplitter())
                {
                    if (mixed_item_nodes.find(node) != mixed_item_nodes.end()) continue;
                    OrganizerNode* org = static_cast<OrganizerNode*>(node);
                    if (org->item == nullptr) org->ChangeItem(belt_item[bi]);
                }
            }
        }

        // Upstream item propagation from manufacturer inputs.
        //
        // Seeding only resolves items at single-in / single-out machines, so a
        // belt feeding a MULTI-input manufacturer (Computer, Heavy Modular
        // Frame, Steel Ingot, ...) is left unresolved — and so is everything
        // upstream of it: the splitters/mergers and, crucially, the truck/train
        // station that unloads cargo into that line. With no item of their own,
        // those pins get back-filled with whatever item happens to be nearby
        // (often the manufacturer's *production*), which is exactly the reported
        // "truck station shows the constructor's output" bug.
        //
        // Each CraftNode input pin already carries its recipe ingredient (and
        // item-aware routing during wiring put the right ingredient on the right
        // pin). That is the authoritative item for everything feeding it, so we
        // walk upstream from every craft input pin and stamp the ingredient onto
        // the pass-through pins along the way. We stop at craft/extractor pins
        // (their items are recipe/resource-bound) and at smart/programmable
        // splitters (their branches may legitimately carry different items), and
        // never touch a station's fuel inlet.
        for (const auto& node : out.nodes)
        {
            if (!node->IsCraft()) continue;
            for (const auto& in_pin : node->ins)
            {
                if (in_pin->item == nullptr || in_pin->link == nullptr) continue;
                const Item* ingredient = in_pin->item;
                std::vector<Pin*> stack;
                std::unordered_set<Pin*> seen;
                stack.push_back(in_pin->link->start);
                while (!stack.empty())
                {
                    Pin* p = stack.back();
                    stack.pop_back();
                    if (p == nullptr || !seen.insert(p).second) continue;
                    Node* pn = p->node;
                    if (pn == nullptr) continue;
                    if (pn->IsCustomSplitter()) continue;       // item-separation wall
                    if (!pn->IsOrganizer() && !pn->IsLogistics()) continue; // authoritative producer
                    if (mixed_item_nodes.find(pn) != mixed_item_nodes.end()) continue;
                    // Only fill in items that are still UNKNOWN. An organizer
                    // already typed from its own upstream producer (the belt_item
                    // seed/propagation passes above) is authoritative — upstream
                    // supply wins over downstream demand. Overriding it would let
                    // a multi-input craft's ingredient (e.g. a Circuit Board's
                    // Copper Sheet pin, reached by rank because the belt's real
                    // item had no matching pin) repaint an entire Plastic chain as
                    // Copper Sheet and flood that wrong item through the manifold.
                    if (pn->IsOrganizer())
                    {
                        OrganizerNode* org = static_cast<OrganizerNode*>(pn);
                        if (org->item == nullptr) org->ChangeItem(ingredient);
                        else if (org->item != ingredient) continue; // typed differently: don't cross it
                    }
                    else // logistics / station: stamp only the untyped cargo pins
                    {
                        for (auto& q : pn->ins)  { if (!IsStationFuelPin(pn, q.get()) && q->item == nullptr) q->item = ingredient; }
                        for (auto& q : pn->outs) { if (q->item == nullptr) q->item = ingredient; }
                    }
                    // Continue upstream through this node's (cargo) inputs.
                    for (auto& q : pn->ins)
                    {
                        if (q->link != nullptr && !IsStationFuelPin(pn, q.get()))
                        {
                            stack.push_back(q->link->start);
                        }
                    }
                }
            }
        }

        // ---- Step: vehicle route links (opt-in) ----
        // Wire each recorded vehicle route as station→station transport edges.
        // A station that a factory belt FEEDS is a loader (cargo goes out on the
        // truck); a station that FEEDS a factory belt is an unloader (the truck
        // brought cargo in). We connect a loader's free output pin to an
        // unloader's free input pin, carrying the item/rate already resolved on
        // the belt side. Conservative: legs with no spare pin or a clashing item
        // are skipped and reported rather than forced. Inserted before the rate
        // passes so the new edges propagate like any other link.
        if (options.connect_vehicle_routes && !parsed.vehicle_routes.empty())
        {
            auto is_station_node = [](const Node* n) {
                if (n == nullptr || !n->IsLogistics()) return false;
                const LogisticsNode* l = static_cast<const LogisticsNode*>(n);
                return l->logistics_kind == LogisticsNode::Kind::TruckStation
                    || l->logistics_kind == LogisticsNode::Kind::TrainStation;
            };

            struct StationWire
            {
                Node* node = nullptr;
                bool is_loader = false;    // factory belt feeds it (cargo input wired)
                bool is_unloader = false;  // it feeds a factory belt (cargo output wired)
                const Item* load_item = nullptr;
                const Item* unload_item = nullptr;
                FractionalNumber load_rate{ 0, 1 };
            };

            // A loader station is a downstream endpoint (cargo leaves on the
            // truck), so the upstream item-propagation pass never visits it and
            // its input pin item may still be null. Resolve the carried item by
            // walking UP from the input pin to the first typed producer pin
            // (organizer items have been stamped by the passes above).
            auto resolve_item_upstream = [](Pin* pin) -> const Item* {
                std::unordered_set<Pin*> seen;
                Pin* cur = pin;
                for (int hop = 0; hop < 64 && cur != nullptr; ++hop)
                {
                    if (cur->item != nullptr) return cur->item;
                    if (cur->link == nullptr) return nullptr;
                    Pin* up = cur->link->start;
                    if (up == nullptr || !seen.insert(up).second) return nullptr;
                    if (up->item != nullptr) return up->item;
                    Node* n = up->node;
                    if (n == nullptr) return nullptr;
                    Pin* next = nullptr;
                    for (const auto& q : n->ins) { if (q->link != nullptr) { next = q.get(); break; } }
                    cur = next;
                }
                return nullptr;
            };

            std::unordered_map<std::string, StationWire> wires;
            for (const LogisticsStation& ls : parsed.logistics_stations)
            {
                auto idx = id_to_index.find(ls.id);
                if (idx == id_to_index.end()) continue;
                Node* node = out.nodes[idx->second].get();
                if (!is_station_node(node)) continue;
                StationWire w;
                w.node = node;
                for (const auto& p : node->ins)
                {
                    if (IsStationFuelPin(node, p.get())) continue;
                    if (p->link != nullptr)
                    {
                        w.is_loader = true;
                        if (w.load_item == nullptr) { w.load_item = resolve_item_upstream(p.get()); w.load_rate = p->current_rate; }
                    }
                }
                for (const auto& p : node->outs)
                {
                    if (p->link != nullptr)
                    {
                        w.is_unloader = true;
                        if (w.unload_item == nullptr) w.unload_item = p->item;
                    }
                }
                wires[ls.id] = w;
            }

            // Stations are now allocated with only their connected cargo pins, so
            // a loader may have no spare output (and an unloader no spare cargo
            // input) for the vehicle leg. Reuse a free pin if one exists, else
            // grow one. New cargo inputs are inserted BEFORE the fuel pin so it
            // stays last (IsStationFuelPin relies on that).
            auto free_out_pin = [&](Node* n) -> Pin* {
                for (const auto& p : n->outs) if (p->link == nullptr) return p.get();
                n->outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, n, nullptr));
                return n->outs.back().get();
            };
            auto free_in_pin = [&](Node* n) -> Pin* {
                for (const auto& p : n->ins)
                {
                    if (IsStationFuelPin(n, p.get())) continue;
                    if (p->link == nullptr) return p.get();
                }
                auto pin = std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, n, nullptr);
                Pin* raw = pin.get();
                if (!n->ins.empty()) n->ins.insert(n->ins.end() - 1, std::move(pin)); // keep fuel pin last
                else n->ins.push_back(std::move(pin));
                return raw;
            };

            std::set<std::pair<const void*, const void*>> created;
            size_t route_links = 0, legs_no_pin = 0, legs_item_mismatch = 0;

            for (const std::vector<std::string>& route : parsed.vehicle_routes)
            {
                std::vector<StationWire*> loaders, unloaders;
                for (const std::string& sid : route)
                {
                    auto it = wires.find(sid);
                    if (it == wires.end()) continue;
                    if (it->second.is_loader) loaders.push_back(&it->second);
                    if (it->second.is_unloader) unloaders.push_back(&it->second);
                }
                for (StationWire* loader : loaders)
                {
                    for (StationWire* unloader : unloaders)
                    {
                        if (loader == unloader || loader->node == unloader->node) continue;
                        const Item* item = loader->load_item;
                        if (item == nullptr) continue; // can't type the leg
                        if (unloader->unload_item != nullptr && unloader->unload_item != item)
                        {
                            legs_item_mismatch += 1;
                            continue;
                        }
                        const auto key = std::make_pair(
                            static_cast<const void*>(loader->node), static_cast<const void*>(unloader->node));
                        if (!created.insert(key).second) continue;
                        Pin* op = free_out_pin(loader->node);
                        Pin* ip = free_in_pin(unloader->node);
                        if (op == nullptr || ip == nullptr) { legs_no_pin += 1; continue; }
                        op->item = item;
                        ip->item = item;
                        op->current_rate = loader->load_rate;
                        ip->current_rate = loader->load_rate;
                        out.links.emplace_back(std::make_unique<Link>(id_generator(), op, ip));
                        op->link = out.links.back().get();
                        ip->link = out.links.back().get();
                        route_links += 1;
                    }
                }
            }

            std::string msg = "[vehicle routes] " + std::to_string(route_links) + " station link(s) created";
            if (legs_no_pin > 0) msg += ", " + std::to_string(legs_no_pin) + " leg(s) skipped (no free pin)";
            if (legs_item_mismatch > 0) msg += ", " + std::to_string(legs_item_mismatch) + " leg(s) skipped (item mismatch)";
            out.warnings.push_back(msg);
        }

        // Forward-propagate rates from producers (CraftNode/Extractor outputs
        // have current_rate set by UpdateRate above) through the wired graph.
        // We don't invoke the full equation solver here because that lives on
        // ProductionApp; this best-effort pass handles the common chains so the
        // user sees correct rates on storages, stations, sinks, and the
        // organizers in between right after import:
        //   - Merger output  = sum(inputs)
        //   - Splitter outs  = input / count(connected outputs)
        //   - Logistics outs = sum(inputs) / count(connected outputs)
        //   - Link propagation copies upstream output rate onto the downstream
        //     input pin, except for CraftNode/Extractor pins, whose rates come
        //     from the recipe and must not be overwritten.
        // Iterates to a fixed point so multi-hop chains converge.
        constexpr int kMaxRatePropagationIters = 64;
        for (int iter = 0; iter < kMaxRatePropagationIters; ++iter)
        {
            bool changed = false;

            for (const auto& node : out.nodes)
            {
                if (node->IsMerger())
                {
                    FractionalNumber sum(0, 1);
                    for (const auto& p : node->ins) sum = sum + p->current_rate;
                    Pin* op = node->outs[0].get();
                    if (op->current_rate != sum)
                    {
                        op->current_rate = sum;
                        changed = true;
                    }
                }
                else if (node->IsGameSplitter() || node->IsCustomSplitter())
                {
                    size_t connected_outs = 0;
                    for (const auto& p : node->outs)
                    {
                        if (p->link != nullptr) connected_outs += 1;
                    }
                    if (connected_outs == 0) continue;
                    const FractionalNumber per_out = node->ins[0]->current_rate
                        / FractionalNumber(static_cast<long long>(connected_outs));
                    for (const auto& p : node->outs)
                    {
                        if (p->link == nullptr) continue;
                        if (p->current_rate != per_out)
                        {
                            p->current_rate = per_out;
                            changed = true;
                        }
                    }
                }
                else if (node->IsLogistics())
                {
                    FractionalNumber sum(0, 1);
                    const Item* in_item = nullptr;
                    for (const auto& p : node->ins)
                    {
                        if (IsStationFuelPin(node.get(), p.get())) continue;
                        sum = sum + p->current_rate;
                        if (in_item == nullptr) in_item = p->item;
                    }
                    size_t connected_outs = 0;
                    for (const auto& p : node->outs)
                    {
                        if (p->link != nullptr) connected_outs += 1;
                    }
                    if (connected_outs == 0) continue;
                    const FractionalNumber per_out = sum
                        / FractionalNumber(static_cast<long long>(connected_outs));
                    for (const auto& p : node->outs)
                    {
                        if (p->link == nullptr) continue;
                        if (p->current_rate != per_out)
                        {
                            p->current_rate = per_out;
                            changed = true;
                        }
                        // Pass-through item: a storage / station's output
                        // carries whatever its inputs carry. Without this the
                        // last storage in a chain ends up displaying the item
                        // of whatever non-logistics node it happens to feed.
                        if (mixed_item_nodes.find(node.get()) == mixed_item_nodes.end()
                            && in_item != nullptr
                            && p->item != in_item)
                        {
                            p->item = in_item;
                            changed = true;
                        }
                    }
                }
            }

            for (const auto& link : out.links)
            {
                if (link->start == nullptr || link->end == nullptr) continue;
                Pin* upstream = link->start;
                Pin* downstream = link->end;
                if (downstream->node->IsCraft() || downstream->node->IsExtractor()) continue;
                if (downstream->current_rate != upstream->current_rate)
                {
                    downstream->current_rate = upstream->current_rate;
                    changed = true;
                }
                // Item: only copy upstream onto downstream when the downstream
                // is a logistics input. Organizer pins are handled by their
                // own ChangeItem step earlier; CraftNode/Extractor/Sink keep
                // their recipe/resource-bound items.
                if (downstream->node->IsLogistics()
                    && upstream->item != nullptr
                    && downstream->item != upstream->item)
                {
                    downstream->item = upstream->item;
                    changed = true;
                }
            }

            if (!changed) break;
        }

        // ---- Step: connection consistency / serial pass-through validation ----
        // The forward pass above distributes a splitter's (and storage's) input
        // *equally* across its outputs and never writes a craft's input rate.
        // Real factories split by demand, so that leaves many links whose two
        // endpoints carry different rates — exactly the condition the editor
        // paints red — and the error compounds along serial splitter / merger /
        // storage chains. This step pulls each consumer's demanded rate back
        // through those pass-through nodes so every internal link in a chain
        // carries one consistent rate. Craft/Extractor pins are recipe-fixed and
        // never touched; only flexible logistics/organizer pins move. Two
        // directions are kept stable by giving each pin a single authority:
        //   - splitter outputs / storage outputs  ← downstream demand
        //   - splitter input / storage inputs      = sum of those outputs
        //   - merger output                        = sum of its inputs
        //   - merger / sink inputs                 ← upstream supply (link copy)
        // A genuine imbalance (producer supply != total downstream demand) is
        // left as a single red link at the producer boundary and reported below.
        for (int iter = 0; iter < kMaxRatePropagationIters; ++iter)
        {
            bool changed = false;
            for (const auto& node : out.nodes)
            {
                if (node->IsGameSplitter() || node->IsCustomSplitter())
                {
                    FractionalNumber total(0, 1);
                    for (const auto& p : node->outs)
                    {
                        if (p->link == nullptr || p->link->end == nullptr) continue;
                        const FractionalNumber demand = p->link->end->current_rate;
                        if (p->current_rate != demand) { p->current_rate = demand; changed = true; }
                        total = total + demand;
                    }
                    if (!node->ins.empty() && node->ins[0]->current_rate != total)
                    {
                        node->ins[0]->current_rate = total;
                        changed = true;
                    }
                }
                else if (node->IsLogistics())
                {
                    FractionalNumber total(0, 1);
                    size_t connected_outs = 0;
                    for (const auto& p : node->outs)
                    {
                        if (p->link == nullptr || p->link->end == nullptr) continue;
                        const FractionalNumber demand = p->link->end->current_rate;
                        if (p->current_rate != demand) { p->current_rate = demand; changed = true; }
                        total = total + demand;
                        connected_outs += 1;
                    }
                    if (connected_outs == 0) continue;
                    size_t connected_ins = 0;
                    for (const auto& p : node->ins)
                    {
                        if (p->link != nullptr && !IsStationFuelPin(node.get(), p.get())) connected_ins += 1;
                    }
                    if (connected_ins == 0) continue;
                    const FractionalNumber per_in = total
                        / FractionalNumber(static_cast<long long>(connected_ins));
                    for (const auto& p : node->ins)
                    {
                        if (p->link == nullptr || IsStationFuelPin(node.get(), p.get())) continue;
                        if (p->current_rate != per_in) { p->current_rate = per_in; changed = true; }
                    }
                }
                else if (node->IsMerger())
                {
                    FractionalNumber sum(0, 1);
                    for (const auto& p : node->ins) sum = sum + p->current_rate;
                    if (!node->outs.empty() && node->outs[0]->current_rate != sum)
                    {
                        node->outs[0]->current_rate = sum;
                        changed = true;
                    }
                }
            }
            // Supply-driven consumer inputs follow their upstream output. Craft/
            // Extractor inputs are recipe-fixed, and splitter/storage inputs are
            // owned by the demand-conservation rules above, so both are skipped
            // here to avoid two rules fighting over one pin (which would never
            // converge).
            for (const auto& link : out.links)
            {
                if (link->start == nullptr || link->end == nullptr) continue;
                Node* dn = link->end->node;
                if (!(dn->IsMerger() || dn->IsSink())) continue;
                if (link->end->current_rate != link->start->current_rate)
                {
                    link->end->current_rate = link->start->current_rate;
                    changed = true;
                }
            }
            if (!changed) break;
        }

        // Verify every connection: both endpoints equal == the editor draws it
        // green. Any residual mismatch is now a real supply/demand imbalance in
        // the imported factory, not an import artifact, so surface the count.
        size_t inconsistent_links = 0;
        for (const auto& link : out.links)
        {
            if (link->start == nullptr || link->end == nullptr) continue;
            if (link->start->current_rate != link->end->current_rate) inconsistent_links += 1;
        }
        if (inconsistent_links > 0)
        {
            out.warnings.push_back(std::to_string(inconsistent_links) +
                " connection(s) rate-inconsistent after balancing (over/under-production)");
        }

        if (connected > 0)
        {
            out.warnings.push_back(std::to_string(connected) + " belt connection(s) wired");
        }
        if (wrong_side_belts > 0)
        {
            out.warnings.push_back(std::to_string(wrong_side_belts) +
                " belt(s) skipped: wrong-side connection (one-way belt direction mismatch)");
        }
        if (port_out_of_range > 0)
        {
            out.warnings.push_back(std::to_string(port_out_of_range) +
                " belt(s) skipped: port index outside node pin range");
        }
        if (collisions > 0)
        {
            out.warnings.push_back(std::to_string(collisions) +
                " belt(s) skipped: would collide with an existing connection");
        }

        ResolveExtractorResources(out.nodes, id_generator);

        if (options.layout_mode == LayoutMode::Compact)
        {
            ApplyCompactLayout(out.nodes, out.links);
        }

        return true;
    }
}
