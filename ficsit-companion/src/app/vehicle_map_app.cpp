#include "app/vehicle_map_app.hpp"

#include "domain/gamedata/game_data.hpp"
#include "domain/core/json.hpp"
#include "domain/gamedata/recipe.hpp" // struct Item
#include "domain/vehicle/vehicle_map_query.hpp"
#include "infra/saveimport/sav_import_service.hpp"
#include "infra/saveimport/sav_runner.hpp"
#include "infra/persistence/vehicle_map_session.hpp"
#include "app/utils.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#endif

namespace
{
    const char* kSessionFile = "saved/vehicle_map.json";

    unsigned int TypeBit(VehicleMap::VehicleType t)
    {
        return 1u << static_cast<unsigned int>(t);
    }

    // Stable, reasonably-distinct color from an arbitrary string (item legend mode).
    ImU32 HashColor(const std::string& s)
    {
        unsigned int h = 2166136261u;
        for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }
        // Spread into a pleasant range: vary hue, keep value/saturation high.
        const float hue = (h % 360u) / 360.0f;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 0.95f, r, g, b);
        return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), 255);
    }

    ImU32 VehicleColor(VehicleMap::VehicleType t)
    {
        switch (t)
        {
        case VehicleMap::VehicleType::Truck:      return IM_COL32(255, 196, 64, 255);
        case VehicleMap::VehicleType::Tractor:    return IM_COL32(120, 220, 120, 255);
        case VehicleMap::VehicleType::Explorer:   return IM_COL32(120, 200, 255, 255);
        case VehicleMap::VehicleType::CyberWagon: return IM_COL32(220, 130, 255, 255);
        case VehicleMap::VehicleType::Train:      return IM_COL32(255, 120, 120, 255);
        default:                                  return IM_COL32(200, 200, 200, 255);
        }
    }

    ImU32 WithAlpha(ImU32 c, float a)
    {
        const int alpha = std::max(0, std::min(255, static_cast<int>(a * 255.0f)));
        return (c & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
    }
}

VehicleMapApp::VehicleMapApp()
{
    LoadSession();
    // .sav loading (folder watching, world filtering) is owned by the shared
    // AppHost load bar now; this tool only receives parsed saves via
    // LoadFromWrapperJson.
}

VehicleMapApp::~VehicleMapApp()
{
    SaveSession();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

std::string VehicleMapApp::Serialize() const
{
    // Gather live state (camera + members) into the serializable snapshot.
    VehicleMapSession s;
    s.node_executable_path = node_executable_path;
    s.sav_watch_dir = sav_watch_dir;
    s.sav_watch_world = sav_watch_world;
    s.sav_watch_enabled = sav_watch_enabled;
    s.last_sav_path = last_sav_path;
    s.pan = camera.Pan();
    s.zoom = camera.Zoom();
    s.sel_station = sel_station;
    s.sel_vehicle = sel_vehicle;
    s.sel_item = sel_item;
    s.search = search;
    s.type_mask = type_mask;
    s.layers.roads = layers.roads;
    s.layers.rails = layers.rails;
    s.layers.vehicles = layers.vehicles;
    s.layers.stations = layers.stations;
    s.layers.labels = layers.labels;
    s.color_by_item = color_by_item;
    return s.Serialize();
}

void VehicleMapApp::Deserialize(const std::string& str)
{
    // Parse into the snapshot (seeded with current state so absent keys keep
    // their values), then scatter back into live state.
    VehicleMapSession s;
    s.node_executable_path = node_executable_path;
    s.sav_watch_dir = sav_watch_dir;
    s.sav_watch_world = sav_watch_world;
    s.sav_watch_enabled = sav_watch_enabled;
    s.last_sav_path = last_sav_path;
    s.pan = camera.Pan();
    s.zoom = camera.Zoom();
    s.sel_station = sel_station;
    s.sel_vehicle = sel_vehicle;
    s.sel_item = sel_item;
    s.search = search;
    s.type_mask = type_mask;
    s.layers.roads = layers.roads;
    s.layers.rails = layers.rails;
    s.layers.vehicles = layers.vehicles;
    s.layers.stations = layers.stations;
    s.layers.labels = layers.labels;
    s.color_by_item = color_by_item;

    s.Deserialize(str);

    node_executable_path = s.node_executable_path;
    sav_watch_dir = s.sav_watch_dir;
    sav_watch_world = s.sav_watch_world;
    sav_watch_enabled = s.sav_watch_enabled;
    last_sav_path = s.last_sav_path;
    camera.SetPan(s.pan);
    camera.SetZoom(s.zoom);
    sel_station = s.sel_station;
    sel_vehicle = s.sel_vehicle;
    sel_item = s.sel_item;
    search = s.search;
    type_mask = s.type_mask;
    layers.roads = s.layers.roads;
    layers.rails = s.layers.rails;
    layers.vehicles = s.layers.vehicles;
    layers.stations = s.layers.stations;
    layers.labels = s.layers.labels;
    color_by_item = s.color_by_item;
}

void VehicleMapApp::LoadSession()
{
#if !defined(__EMSCRIPTEN__)
    std::ifstream f(kSessionFile, std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    Deserialize(ss.str());
#endif
}

void VehicleMapApp::SaveSession()
{
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::create_directories("saved", ec);
    std::ofstream f(kSessionFile, std::ios::binary | std::ios::trunc);
    if (f) f << Serialize();
#endif
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void VehicleMapApp::LoadFromWrapperJson(const std::string& wrapper_json,
                                        const SavImport::BuildOptions& options)
{
    (void)options; // logistics parse is layout-independent
    last_error.clear();
    VehicleMap::Model parsed = VehicleMap::ParseLogisticsJson(wrapper_json);
    if (!parsed.ok)
    {
        last_error = parsed.error.empty() ? "Failed to parse logistics data" : parsed.error;
        return;
    }

    // Pull the wrapper's logistics warning line(s) for diagnostics.
    last_warnings = VehicleMap::ExtractLogisticsWarnings(wrapper_json);

    model = std::move(parsed);
    needs_fit_on_load = !model.has_bounds ? false : true;

    // Drop selections that no longer exist.
    if (!sel_station.empty() && model.station_index.find(sel_station) == model.station_index.end()) sel_station.clear();
    if (!sel_vehicle.empty() && model.vehicle_index.find(sel_vehicle) == model.vehicle_index.end()) sel_vehicle.clear();

    std::ostringstream st;
    st << model.stations.size() << " stations, " << model.vehicles.size()
       << " vehicles, " << model.segments.size() << " path segments";
    status_text = st.str();
}

// ---------------------------------------------------------------------------
// Camera / transform
// ---------------------------------------------------------------------------

// These forward to VehicleMapCamera (the pure transform/fit/fly-to logic now
// lives there); the app supplies model bounds and the per-frame delta time.
ImVec2 VehicleMapApp::WorldToScreen(const ImVec2& world) const
{
    return camera.WorldToScreen(world);
}

ImVec2 VehicleMapApp::ScreenToWorld(const ImVec2& screen) const
{
    return camera.ScreenToWorld(screen);
}

void VehicleMapApp::FitAll()
{
    camera.FitAll(model.world_min, model.world_max, model.has_bounds);
}

void VehicleMapApp::ResetView()
{
    FitAll();
}

void VehicleMapApp::StartFlyTo(const ImVec2& world_pos, float target_zoom)
{
    camera.StartFlyTo(world_pos, target_zoom);
}

void VehicleMapApp::UpdateFlyTo()
{
    camera.UpdateFlyTo(ImGui::GetIO().DeltaTime);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void VehicleMapApp::SelectStation(const std::string& id, bool fly)
{
    sel_station = id;
    sel_vehicle.clear();
    scroll_to_station = id;
    auto it = model.station_index.find(id);
    if (fly && it != model.station_index.end())
    {
        StartFlyTo(model.stations[it->second].pos, std::max(camera.Zoom(), 0.08f));
    }
}

void VehicleMapApp::SelectVehicle(const std::string& id, bool fly)
{
    sel_vehicle = id;
    sel_station.clear();
    scroll_to_vehicle = id;
    auto it = model.vehicle_index.find(id);
    if (fly && it != model.vehicle_index.end())
    {
        StartFlyTo(model.vehicles[it->second].pos, std::max(camera.Zoom(), 0.08f));
    }
}

void VehicleMapApp::ClearSelection()
{
    sel_station.clear();
    sel_vehicle.clear();
}

// ---------------------------------------------------------------------------
// Filtering / highlight
// ---------------------------------------------------------------------------

// Filter predicates forward to the pure VehicleMapQuery functions, binding the
// app's current search / type-mask state.
bool VehicleMapApp::StationPassesFilter(const VehicleMap::Station& st) const
{
    return VehicleMapQuery::StationPassesFilter(st, search);
}

bool VehicleMapApp::VehiclePassesFilter(const VehicleMap::Vehicle& ve) const
{
    return VehicleMapQuery::VehiclePassesFilter(ve, search, type_mask);
}

// ---------------------------------------------------------------------------
// Top-level render
// ---------------------------------------------------------------------------

void VehicleMapApp::RenderImpl()
{
    // Saves arrive via LoadFromWrapperJson from the shared global load bar; there
    // is no per-tool load or watch loop here anymore.
    UpdateFlyTo();

    // Keyboard shortcuts (when not typing in a text field).
    if (!ImGui::GetIO().WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { ClearSelection(); sel_item.clear(); }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) FitAll();
    }

    RenderLeftPanel();
    ImGui::SameLine();
    RenderCanvas();
    RenderControlsPopup();
}

// ---------------------------------------------------------------------------
// Left panel
// ---------------------------------------------------------------------------

void VehicleMapApp::RenderLeftPanel()
{
    ImGui::BeginChild("##vm_left", ImVec2(330.0f, 0.0f), true);

    ImGui::TextUnformatted("Vehicle & Station Map");
    ImGui::SameLine();
    if (ImGui::SmallButton("?")) ImGui::OpenPopup("Vehicle Map Controls");
    ImGui::Separator();

    RenderLoadSection();
    ImGui::Separator();
    RenderFilterBar();
    ImGui::Separator();
    RenderIssuesPanel();
    RenderEntityLists();
    ImGui::Separator();
    RenderDetailPanel();

    ImGui::EndChild();
}

void VehicleMapApp::RenderLoadSection()
{
    // The .sav load controls live in the single global load bar at the top of the
    // window (it feeds every tool). This is read-only status for the last import.
    if (!ImGui::CollapsingHeader("Load status", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (model.stations.empty() && model.vehicles.empty() && status_text.empty() && last_error.empty())
    {
        ImGui::TextDisabled("Load a save from the bar above to populate this map.");
        return;
    }

    if (!status_text.empty()) ImGui::TextDisabled("%s", status_text.c_str());
    if (!last_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", last_error.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy##vm_err")) ImGui::SetClipboardText(last_error.c_str());
    }
}

void VehicleMapApp::RenderFilterBar()
{
    ImGui::TextUnformatted("Filter");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##vm_search", "Search name / item...", &search);

    // Vehicle type filter.
    auto type_checkbox = [&](const char* label, VehicleMap::VehicleType t) {
        bool on = (type_mask & TypeBit(t)) != 0;
        if (ImGui::Checkbox(label, &on))
        {
            if (on) type_mask |= TypeBit(t);
            else type_mask &= ~TypeBit(t);
        }
    };
    type_checkbox("Truck", VehicleMap::VehicleType::Truck);
    ImGui::SameLine(); type_checkbox("Tractor", VehicleMap::VehicleType::Tractor);
    type_checkbox("Explorer", VehicleMap::VehicleType::Explorer);
    ImGui::SameLine(); type_checkbox("Cyber Wagon", VehicleMap::VehicleType::CyberWagon);
    type_checkbox("Train", VehicleMap::VehicleType::Train);

    // Item-centric query combo.
    const std::string preview = sel_item.empty() ? std::string("Highlight item: (none)") : ("Highlight item: " + sel_item);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##vm_item", preview.c_str()))
    {
        if (ImGui::Selectable("(none)", sel_item.empty())) sel_item.clear();
        for (const std::string& name : model.item_names_sorted)
        {
            if (ImGui::Selectable(name.c_str(), sel_item == name)) sel_item = name;
        }
        ImGui::EndCombo();
    }
    if (!sel_item.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##vm_item_clear")) sel_item.clear();
    }

    // Layers + legend toggles.
    ImGui::Checkbox("Roads", &layers.roads); ImGui::SameLine();
    ImGui::Checkbox("Stations", &layers.stations); ImGui::SameLine();
    ImGui::Checkbox("Vehicles", &layers.vehicles);
    ImGui::Checkbox("Labels", &layers.labels); ImGui::SameLine();
    ImGui::Checkbox("Color by item", &color_by_item);

    if (ImGui::Button("Fit all (F)")) FitAll();
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) ResetView();
}

void VehicleMapApp::RenderIssuesPanel()
{
    const size_t issues = model.orphan_stations.size() + model.orphan_vehicles.size();
    if (issues == 0) return;
    char label[64];
    std::snprintf(label, sizeof(label), "Issues (%d)###vm_issues", static_cast<int>(issues));
    if (!ImGui::CollapsingHeader(label)) return;

    for (size_t si : model.orphan_stations)
    {
        const VehicleMap::Station& st = model.stations[si];
        ImGui::PushID(static_cast<int>(si));
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "[no vehicle]");
        ImGui::SameLine();
        if (ImGui::SmallButton(st.name.empty() ? "(unnamed)" : st.name.c_str())) SelectStation(st.id, true);
        ImGui::PopID();
    }
    for (size_t vi : model.orphan_vehicles)
    {
        const VehicleMap::Vehicle& ve = model.vehicles[vi];
        ImGui::PushID(static_cast<int>(vi) + 100000);
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "[no route]");
        ImGui::SameLine();
        if (ImGui::SmallButton(ve.name.empty() ? "(vehicle)" : ve.name.c_str())) SelectVehicle(ve.id, true);
        ImGui::PopID();
    }
}

void VehicleMapApp::RenderEntityLists()
{
    if (ImGui::CollapsingHeader("Stations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::BeginChild("##vm_stations", ImVec2(0.0f, 150.0f), true);
        for (size_t i = 0; i < model.stations.size(); ++i)
        {
            const VehicleMap::Station& st = model.stations[i];
            if (!StationPassesFilter(st)) continue;
            ImGui::PushID(static_cast<int>(i));
            const bool selected = (sel_station == st.id);
            if (ImGui::Selectable(st.name.empty() ? "(unnamed station)" : st.name.c_str(), selected))
            {
                SelectStation(st.id, true);
            }
            if (ImGui::IsItemHovered()) hover_station = st.id;
            if (selected && !scroll_to_station.empty()) { ImGui::SetScrollHereY(); scroll_to_station.clear(); }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Vehicles", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::BeginChild("##vm_vehicles", ImVec2(0.0f, 150.0f), true);
        for (size_t i = 0; i < model.vehicles.size(); ++i)
        {
            const VehicleMap::Vehicle& ve = model.vehicles[i];
            if (!VehiclePassesFilter(ve)) continue;
            ImGui::PushID(static_cast<int>(i) + 200000);
            const bool selected = (sel_vehicle == ve.id);
            char label[160];
            std::snprintf(label, sizeof(label), "%s  (%s)",
                ve.name.empty() ? "(vehicle)" : ve.name.c_str(), VehicleMap::VehicleTypeName(ve.type));
            if (ImGui::Selectable(label, selected)) SelectVehicle(ve.id, true);
            if (ImGui::IsItemHovered()) hover_vehicle = ve.id;
            if (selected && !scroll_to_vehicle.empty()) { ImGui::SetScrollHereY(); scroll_to_vehicle.clear(); }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

void VehicleMapApp::RenderDetailPanel()
{
    auto draw_items = [&](const std::vector<std::string>& names) {
        if (names.empty()) { ImGui::TextDisabled("  (no cargo items)"); return; }
        for (const std::string& n : names) ImGui::BulletText("%s", n.c_str());
    };

    if (!sel_station.empty())
    {
        auto it = model.station_index.find(sel_station);
        if (it == model.station_index.end()) return;
        const VehicleMap::Station& st = model.stations[it->second];
        ImGui::Text("Station: %s", st.name.empty() ? "(unnamed)" : st.name.c_str());
        ImGui::TextDisabled("%s station", st.kind == VehicleMap::StationKind::Train ? "Train" : "Truck");
        ImGui::TextUnformatted("Items:");
        draw_items(st.item_names);
        ImGui::Text("Vehicles serving (%d):", static_cast<int>(st.vehicle_ids.size()));
        for (const std::string& vid : st.vehicle_ids)
        {
            auto vit = model.vehicle_index.find(vid);
            if (vit == model.vehicle_index.end()) continue;
            const VehicleMap::Vehicle& ve = model.vehicles[vit->second];
            ImGui::PushID(vid.c_str());
            if (ImGui::SmallButton(ve.name.empty() ? "(vehicle)" : ve.name.c_str())) SelectVehicle(ve.id, true);
            ImGui::PopID();
        }
    }
    else if (!sel_vehicle.empty())
    {
        auto it = model.vehicle_index.find(sel_vehicle);
        if (it == model.vehicle_index.end()) return;
        const VehicleMap::Vehicle& ve = model.vehicles[it->second];
        ImGui::Text("Vehicle: %s", ve.name.empty() ? "(vehicle)" : ve.name.c_str());
        ImGui::TextDisabled("%s%s", VehicleMap::VehicleTypeName(ve.type), ve.autopilot ? " - autopilot" : "");
        if (!ve.fuel_name.empty()) ImGui::Text("Fuel: %s", ve.fuel_name.c_str());
        ImGui::Text("Route stops (%d):", static_cast<int>(ve.station_ids.size()));
        for (size_t k = 0; k < ve.station_ids.size(); ++k)
        {
            auto sit = model.station_index.find(ve.station_ids[k]);
            if (sit == model.station_index.end()) continue;
            const VehicleMap::Station& st = model.stations[sit->second];
            ImGui::PushID(static_cast<int>(k));
            char label[160];
            std::snprintf(label, sizeof(label), "%d. %s", static_cast<int>(k + 1),
                st.name.empty() ? "(unnamed)" : st.name.c_str());
            if (ImGui::SmallButton(label)) SelectStation(st.id, true);
            ImGui::PopID();
        }
    }
    else
    {
        ImGui::TextDisabled("Click a station or vehicle to inspect it.");
    }
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

void VehicleMapApp::RenderCanvas()
{
    ImGui::BeginChild("##vm_canvas", ImVec2(0.0f, 0.0f), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 1.0f) canvas_sz.x = 1.0f;
    if (canvas_sz.y < 1.0f) canvas_sz.y = 1.0f;
    camera.SetCanvas(ImGui::GetCursorScreenPos(), canvas_sz);
    const ImVec2 canvas_p0 = camera.CanvasP0();
    const ImVec2 canvas_p1(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    if (needs_fit_on_load) { FitAll(); needs_fit_on_load = false; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(24, 26, 30, 255));
    dl->PushClipRect(canvas_p0, canvas_p1, true);

    // Interaction surface.
    ImGui::InvisibleButton("##vm_canvas_btn", canvas_sz,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    // Pan (drag).
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        camera.PanBy(io.MouseDelta);
    }
    // Zoom about cursor.
    if (hovered && io.MouseWheel != 0.0f)
    {
        camera.ZoomAbout(io.MousePos, io.MouseWheel);
    }
    const float zoom = camera.Zoom();

    // ---- Empty / error state ----
    if (model.Empty())
    {
        const char* msg = !last_error.empty() ? last_error.c_str()
            : (last_sav_path.empty() ? "No save loaded - use Load latest now"
                                     : "No vehicles or stations found in this save");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(canvas_p0.x + (canvas_sz.x - ts.x) * 0.5f,
                           canvas_p0.y + (canvas_sz.y - ts.y) * 0.5f),
                    IM_COL32(170, 170, 170, 255), msg);
        dl->PopClipRect();
        ImGui::EndChild();
        return;
    }

    const VehicleMapQuery::Highlight hl =
        VehicleMapQuery::ComputeHighlight(model, sel_vehicle, sel_station, sel_item);
    const float dim_alpha = 0.12f;

    // ---- Background grid (world-aligned) ----
    {
        const ImVec2 tl = ScreenToWorld(canvas_p0);
        const ImVec2 br = ScreenToWorld(canvas_p1);
        // Choose a world step that yields ~80px grid cells.
        float step = 1000.0f;
        while (step * zoom < 60.0f) step *= 2.0f;
        while (step * zoom > 160.0f) step *= 0.5f;
        const ImU32 grid_col = IM_COL32(255, 255, 255, 12);
        for (float wx = std::floor(tl.x / step) * step; wx < br.x; wx += step)
        {
            const float sx = WorldToScreen(ImVec2(wx, 0.0f)).x;
            dl->AddLine(ImVec2(sx, canvas_p0.y), ImVec2(sx, canvas_p1.y), grid_col);
        }
        for (float wy = std::floor(tl.y / step) * step; wy < br.y; wy += step)
        {
            const float sy = WorldToScreen(ImVec2(0.0f, wy)).y;
            dl->AddLine(ImVec2(canvas_p0.x, sy), ImVec2(canvas_p1.x, sy), grid_col);
        }
    }

    // ---- Road segments ----
    if (layers.roads)
    {
        const int step = zoom < 0.01f ? 3 : (zoom < 0.02f ? 2 : 1); // decimate when zoomed out
        std::vector<ImVec2> pts;
        for (const VehicleMap::Segment& seg : model.segments)
        {
            if (seg.kind == VehicleMap::PathKind::Rail && !layers.rails) continue;
            // Bounding-box cull.
            ImVec2 smin = WorldToScreen(seg.waypoints.front());
            ImVec2 smax = smin;
            for (const ImVec2& w : seg.waypoints)
            {
                const ImVec2 s = WorldToScreen(w);
                smin.x = std::min(smin.x, s.x); smin.y = std::min(smin.y, s.y);
                smax.x = std::max(smax.x, s.x); smax.y = std::max(smax.y, s.y);
            }
            if (smax.x < canvas_p0.x || smin.x > canvas_p1.x ||
                smax.y < canvas_p0.y || smin.y > canvas_p1.y) continue;

            pts.clear();
            for (size_t i = 0; i < seg.waypoints.size(); i += step) pts.push_back(WorldToScreen(seg.waypoints[i]));
            if ((seg.waypoints.size() - 1) % step != 0) pts.push_back(WorldToScreen(seg.waypoints.back()));
            if (pts.size() < 2) continue;

            const bool rail = seg.kind == VehicleMap::PathKind::Rail;
            ImU32 col = rail ? IM_COL32(150, 150, 170, 110) : IM_COL32(110, 120, 130, 90);
            if (hl.active) col = WithAlpha(col, 0.35f);
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, rail ? 2.0f : 1.5f);
        }
    }

    // ---- Highlighted route polylines (station-to-station) ----
    for (const std::vector<ImVec2>& route : hl.routes)
    {
        std::vector<ImVec2> pts;
        pts.reserve(route.size());
        for (const ImVec2& w : route) pts.push_back(WorldToScreen(w));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(255, 230, 120, 220), ImDrawFlags_None, 2.5f);
    }

    // ---- Build marker list (stations + vehicles), with de-overlap ----
    struct Marker { bool is_station; size_t idx; ImVec2 screen; };
    std::vector<Marker> markers;
    markers.reserve(model.stations.size() + model.vehicles.size());

    if (layers.stations)
    {
        for (size_t i = 0; i < model.stations.size(); ++i)
        {
            if (!StationPassesFilter(model.stations[i])) continue;
            markers.push_back({ true, i, WorldToScreen(model.stations[i].pos) });
        }
    }
    if (layers.vehicles)
    {
        for (size_t i = 0; i < model.vehicles.size(); ++i)
        {
            if (!VehiclePassesFilter(model.vehicles[i])) continue;
            markers.push_back({ false, i, WorldToScreen(model.vehicles[i].pos) });
        }
    }

    // De-overlap: fan out markers sharing a screen cell.
    {
        const float cell = 14.0f;
        std::unordered_map<long long, std::vector<size_t>> buckets;
        for (size_t i = 0; i < markers.size(); ++i)
        {
            const long long cx = static_cast<long long>(std::floor(markers[i].screen.x / cell));
            const long long cy = static_cast<long long>(std::floor(markers[i].screen.y / cell));
            buckets[(cx << 32) ^ (cy & 0xffffffffLL)].push_back(i);
        }
        for (auto& [key, group] : buckets)
        {
            (void)key;
            if (group.size() < 2) continue;
            ImVec2 centroid(0, 0);
            for (size_t i : group) { centroid.x += markers[i].screen.x; centroid.y += markers[i].screen.y; }
            centroid.x /= group.size(); centroid.y /= group.size();
            const float r = 8.0f;
            for (size_t k = 0; k < group.size(); ++k)
            {
                const float ang = 6.2831853f * static_cast<float>(k) / static_cast<float>(group.size());
                markers[group[k]].screen = ImVec2(centroid.x + std::cos(ang) * r, centroid.y + std::sin(ang) * r);
            }
        }
    }

    // ---- Hit test (nearest marker under cursor) ----
    hover_station.clear();
    hover_vehicle.clear();
    int hover_marker = -1;
    if (hovered)
    {
        float best = 12.0f * 12.0f;
        for (size_t i = 0; i < markers.size(); ++i)
        {
            const float dx = markers[i].screen.x - io.MousePos.x;
            const float dy = markers[i].screen.y - io.MousePos.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; hover_marker = static_cast<int>(i); }
        }
        if (hover_marker >= 0)
        {
            const Marker& m = markers[hover_marker];
            if (m.is_station) hover_station = model.stations[m.idx].id;
            else hover_vehicle = model.vehicles[m.idx].id;
        }
    }

    // Click / double-click selection (treat as click only if not dragged).
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (std::fabs(drag.x) < 4.0f && std::fabs(drag.y) < 4.0f)
        {
            if (hover_marker >= 0)
            {
                const Marker& m = markers[hover_marker];
                if (m.is_station) SelectStation(model.stations[m.idx].id, false);
                else SelectVehicle(model.vehicles[m.idx].id, false);
            }
            else ClearSelection();
        }
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    }
    if (hovered && hover_marker >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        const Marker& m = markers[hover_marker];
        if (m.is_station) SelectStation(model.stations[m.idx].id, true);
        else SelectVehicle(model.vehicles[m.idx].id, true);
    }

    // ---- Draw markers ----
    for (const Marker& m : markers)
    {
        if (m.is_station)
        {
            const VehicleMap::Station& st = model.stations[m.idx];
            const bool lit = !hl.active || hl.stations.count(st.id) > 0;
            const bool sel = (sel_station == st.id);
            const bool hov = (hover_station == st.id);
            const bool orphan = st.vehicle_ids.empty();
            ImU32 base = color_by_item && !st.item_names.empty()
                ? HashColor(st.item_names.front())
                : (st.kind == VehicleMap::StationKind::Train ? IM_COL32(120, 200, 255, 255) : IM_COL32(90, 200, 160, 255));
            ImU32 col = lit ? base : WithAlpha(base, dim_alpha);
            const float half = 4.5f;
            dl->AddRectFilled(ImVec2(m.screen.x - half, m.screen.y - half),
                              ImVec2(m.screen.x + half, m.screen.y + half), col, 1.5f);
            if (lit)
            {
                dl->AddRect(ImVec2(m.screen.x - half, m.screen.y - half),
                            ImVec2(m.screen.x + half, m.screen.y + half), IM_COL32(0, 0, 0, 200), 1.5f);
            }
            if (orphan && lit)
            {
                dl->AddCircle(m.screen, half + 3.0f, IM_COL32(255, 170, 60, 230), 0, 1.5f);
            }
            if (sel || hov)
            {
                dl->AddRect(ImVec2(m.screen.x - half - 3, m.screen.y - half - 3),
                            ImVec2(m.screen.x + half + 3, m.screen.y + half + 3),
                            IM_COL32(255, 255, 255, sel ? 255 : 160), 2.0f, 0, 2.0f);
            }
        }
        else
        {
            const VehicleMap::Vehicle& ve = model.vehicles[m.idx];
            const bool lit = !hl.active || hl.vehicles.count(ve.id) > 0;
            const bool sel = (sel_vehicle == ve.id);
            const bool hov = (hover_vehicle == ve.id);
            ImU32 base = VehicleColor(ve.type);
            ImU32 col = lit ? base : WithAlpha(base, dim_alpha);
            const float r = 5.5f;
            const ImVec2 a(m.screen.x, m.screen.y - r);
            const ImVec2 b(m.screen.x - r * 0.9f, m.screen.y + r * 0.8f);
            const ImVec2 c(m.screen.x + r * 0.9f, m.screen.y + r * 0.8f);
            dl->AddTriangleFilled(a, b, c, col);
            if (lit) dl->AddTriangle(a, b, c, IM_COL32(0, 0, 0, 200), 1.2f);
            if (sel || hov)
            {
                dl->AddCircle(m.screen, r + 3.0f, IM_COL32(255, 255, 255, sel ? 255 : 160), 0, 2.0f);
            }
        }
    }

    // ---- Labels (station names) ----
    if (layers.labels && zoom > 0.03f)
    {
        for (const Marker& m : markers)
        {
            if (!m.is_station) continue;
            const VehicleMap::Station& st = model.stations[m.idx];
            if (st.name.empty()) continue;
            if (hl.active && hl.stations.count(st.id) == 0) continue;
            dl->AddText(ImVec2(m.screen.x + 7.0f, m.screen.y - 7.0f), IM_COL32(220, 220, 220, 230), st.name.c_str());
        }
    }

    dl->PopClipRect();

    // ---- Tooltip ----
    if (hovered && hover_marker >= 0)
    {
        ImGui::BeginTooltip();
        const Marker& m = markers[hover_marker];
        if (m.is_station)
        {
            const VehicleMap::Station& st = model.stations[m.idx];
            ImGui::Text("%s", st.name.empty() ? "(unnamed station)" : st.name.c_str());
            ImGui::TextDisabled("%s station - %d vehicle(s)",
                st.kind == VehicleMap::StationKind::Train ? "Train" : "Truck",
                static_cast<int>(st.vehicle_ids.size()));
            if (!st.item_names.empty())
            {
                std::string items;
                for (size_t i = 0; i < st.item_names.size(); ++i)
                {
                    if (i) items += ", ";
                    items += st.item_names[i];
                }
                ImGui::TextWrapped("Items: %s", items.c_str());
            }
        }
        else
        {
            const VehicleMap::Vehicle& ve = model.vehicles[m.idx];
            ImGui::Text("%s", ve.name.empty() ? "(vehicle)" : ve.name.c_str());
            ImGui::TextDisabled("%s - %d stop(s)%s", VehicleMap::VehicleTypeName(ve.type),
                static_cast<int>(ve.station_ids.size()), ve.autopilot ? " - autopilot" : "");
            if (!ve.fuel_name.empty()) ImGui::TextDisabled("Fuel: %s", ve.fuel_name.c_str());
        }
        ImGui::EndTooltip();
    }

    // ---- Coordinate readout overlay ----
    if (hovered)
    {
        const ImVec2 w = ScreenToWorld(io.MousePos);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "x %.0f  y %.0f", w.x, w.y);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(canvas_p1.x - ts.x - 8.0f, canvas_p1.y - ts.y - 6.0f),
            IM_COL32(180, 180, 180, 200), buf);
    }

    ImGui::EndChild();
}

void VehicleMapApp::RenderControlsPopup()
{
    if (ImGui::BeginPopup("Vehicle Map Controls"))
    {
        ImGui::TextUnformatted("Vehicle & Station Map");
        ImGui::Separator();
        ImGui::BulletText("Drag: pan       Mouse wheel: zoom");
        ImGui::BulletText("Click marker / list row: select");
        ImGui::BulletText("Double-click: select + fly to");
        ImGui::BulletText("Esc: clear selection & item query");
        ImGui::BulletText("F: fit all in view");
        ImGui::Separator();
        ImGui::TextWrapped("Click a station to see the vehicles that serve it; click a vehicle to see its route stops. Pick an item to highlight its whole logistics network.");
        ImGui::EndPopup();
    }
}
