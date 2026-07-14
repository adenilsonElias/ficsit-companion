#pragma once

#include <string>
#include <vector>

/// @brief Persisted *view* preferences for the Factory Snapshot tool (the shared
/// .sav load config lives in SaveSource, not here). A plain data carrier with
/// JSON (de)serialization, mirroring VehicleMapSession. flow_filter encodes
/// ResourceFlowFilter (0=All, 1=Deficit, 2=Surplus).
struct FactorySnapshotSession
{
    int flow_filter = 0;
    std::string flow_search;
    bool world_layout = false;

    /// @brief Graph-view tuning sliders (Options tab). node_font_scale multiplies
    /// the snapshot node text size [0.5, 3.0]; icon_scale multiplies the product
    /// icon size [0.5, 4.0]; collapsed_font_scale multiplies the zoomed-out (collapsed)
    /// node's input/output number text [0.5, 3.0], independent of node_font_scale.
    /// All default to 1.0 (no change).
    float node_font_scale = 1.0f;
    float icon_scale = 1.0f;
    float collapsed_font_scale = 1.0f;

    /// @brief Snapshot graph-view "hide & bypass logistics" preferences. Master
    /// toggle off by default; per-kind selections default on so enabling the
    /// master immediately hides all four kinds. Visualization-only (model and
    /// resource-flow report are unaffected).
    bool hide_logistics_enabled = false;
    bool hide_game_splitters = true;
    bool hide_custom_splitters = true;
    bool hide_mergers = true;
    bool hide_logistics_nodes = true;

    /// @brief Fold each machine's measured save productivity into its rates so
    /// the snapshot shows real (input-starved / output-blocked) throughput.
    /// Default ON. Toggling re-imports the graph from the retained wrapper JSON.
    bool apply_efficiency = true;
    /// @brief Draw each link's carried items/min on the canvas. Default OFF.
    /// Visualization-only (reads pin rates each frame; no recompute).
    bool show_throughput = false;

    /// @brief Item names whose producers (Craft/Extractor) are hidden on the
    /// snapshot canvas. Empty by default => every production visible. Storing the
    /// *hidden* set (not the visible one) keeps the default empty and makes newly
    /// imported items visible automatically. Visualization-only.
    std::vector<std::string> hidden_production_items;

    std::string Serialize() const;
    /// @brief Per-key merge; malformed JSON is ignored; flow_filter out of
    /// [0,2] is clamped to 0; node_font_scale/icon_scale are clamped to their
    /// allowed ranges.
    void Deserialize(const std::string& json);
};
