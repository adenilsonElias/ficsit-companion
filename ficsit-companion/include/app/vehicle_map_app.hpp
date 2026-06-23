#pragma once

#include <imgui.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "app/base_app.hpp"
#include "domain/vehicle/vehicle_map.hpp"
#include "domain/vehicle/vehicle_map_camera.hpp"

/// @brief Second top-level tool: a 2D world map of vehicles, stations and the
/// routes between them. Fully decoupled from ProductionApp — it shares only the
/// .sav subprocess runner (SavRunner) and the global game data (Data::Items()).
class VehicleMapApp : public BaseApp
{
public:
    VehicleMapApp();
    virtual ~VehicleMapApp() override;

    /// @brief Persist camera / selection / filters / watch config (no ImGui context needed).
    virtual void SaveSession() override;

    /// @brief Parse the shared wrapper JSON's logistics block into the map model.
    /// @param options Shared import options (unused here — the logistics parse is
    /// layout-independent — but kept for the common BaseApp signature).
    virtual void LoadFromWrapperJson(const std::string& wrapper_json,
                                     const SavImport::BuildOptions& options) override;

protected:
    virtual void RenderImpl() override;

private:
    // ---- Persistence ----
    void LoadSession();
    std::string Serialize() const;
    void Deserialize(const std::string& s);

    // ---- Panels ----
    void RenderLeftPanel();
    /// @brief Read-only status line for the last shared import. The load controls
    /// themselves live in the single global load bar (AppHost).
    void RenderLoadSection();
    void RenderFilterBar();
    void RenderEntityLists();
    void RenderIssuesPanel();
    void RenderDetailPanel();
    void RenderCanvas();
    void RenderControlsPopup();

    // ---- Camera / transform ----
    ImVec2 WorldToScreen(const ImVec2& world) const;
    ImVec2 ScreenToWorld(const ImVec2& screen) const;
    void FitAll();
    void ResetView();
    void StartFlyTo(const ImVec2& world_pos, float target_zoom);
    void UpdateFlyTo();

    // ---- Selection ----
    void SelectStation(const std::string& id, bool fly);
    void SelectVehicle(const std::string& id, bool fly);
    void ClearSelection();

    // ---- Filtering / highlight ----
    // (Pure logic lives in domain/vehicle/vehicle_map_query.hpp; these bind app state.)
    bool StationPassesFilter(const VehicleMap::Station& st) const;
    bool VehiclePassesFilter(const VehicleMap::Vehicle& ve) const;

private:
    VehicleMap::Model model;
    VehicleMapCamera camera;

    // Status / diagnostics.
    std::string last_error;
    std::string status_text;
    std::vector<std::string> last_warnings;

    // Persisted settings.
    std::string node_executable_path;
    std::string sav_watch_dir;
    std::string sav_watch_world;
    bool sav_watch_enabled = false;
    std::string last_sav_path;

    // View state (selection + filters; persisted). Pan/zoom live in `camera`.
    std::string sel_station;
    std::string sel_vehicle;
    std::string sel_item;          // active item-centric query (empty = off)
    std::string search;            // free-text filter
    unsigned int type_mask = 0xFFFFFFFFu;
    struct Layers { bool roads = true, rails = true, vehicles = true, stations = true, labels = true; } layers;
    bool color_by_item = false;

    // Transient per-frame canvas hover/scroll state.
    std::string hover_station;
    std::string hover_vehicle;
    std::string scroll_to_station; // request list auto-scroll after a map hover/select
    std::string scroll_to_vehicle;

    bool needs_fit_on_load = false;

    // Background map texture (optional; grid fallback if absent).
    unsigned int background_texture = 0;
    bool background_available = false;
};
