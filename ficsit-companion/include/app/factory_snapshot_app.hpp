#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>

#include <imgui.h>
#include <imgui_node_editor.h>

#include "app/base_app.hpp"
#include "domain/snapshot/factory_snapshot_model.hpp"
#include "domain/snapshot/snapshot_bypass.hpp"
#include "domain/nodes/node_display.hpp"
#include "infra/persistence/factory_snapshot_session.hpp"

/// @brief Third top-level tool: a read-only view of the imported `.sav` factory.
/// It owns a FactorySnapshotModel built from the shared wrapper JSON and renders
/// it list-first: a topology summary, a resource-flow table, and a warnings
/// panel. It exposes no graph-mutation affordances (no add/delete/rate edits).
class FactorySnapshotApp : public BaseApp
{
public:
    FactorySnapshotApp();
    virtual ~FactorySnapshotApp() override;

    virtual void SaveSession() override;
    /// @brief Build the read-only snapshot from the shared wrapper JSON, using
    /// the shared import options so the snapshot matches the other tools.
    virtual void LoadFromWrapperJson(const std::string& wrapper_json,
                                     const SavImport::BuildOptions& options) override;

protected:
    virtual void RenderImpl() override;

private:
    void LoadSession();

    /// @brief Re-run the import from the retained wrapper JSON with the current
    /// session.apply_efficiency, replacing the model in place. Used when the
    /// "Apply efficiency" toggle changes so the whole graph (rates, flow report,
    /// belt throughput) stays consistent with the one importer pipeline. Keeps
    /// the current camera (applies node positions without recentering). No-op if
    /// nothing has been imported yet.
    void RebuildSnapshot();

    void RenderStatusLine();
    void RenderTopologySummary();
    void RenderResourceFlowTable();
    void RenderWarningsPanel();
    /// @brief Options tab: sliders for snapshot node font size and product icon
    /// size (persisted in the session); changes save immediately.
    void RenderOptionsPanel();

    /// @brief Draw the imported factory as a read-only node graph (own editor
    /// context). Applies imported node positions + fits the view on first frame
    /// after a load. No edit/create/delete handlers are wired.
    void RenderGraphCanvas();
    /// @brief Draw one node: category-colored, and either full detail or a
    /// collapsed tile depending on the current zoom. Read-only.
    void RenderSnapshotNode(const Node& node);
    /// @brief Full-detail node body: header product icon + title, per-pin icons +
    /// imported rates, vehicle-station plug. Used at/above the collapse zoom.
    void RenderSnapshotNodeDetailed(const Node& node);
    /// @brief Collapsed node body for zoomed-out views: a large screen-constant
    /// item icon (or a category glyph when there is no item) with all pins still
    /// submitted as 1px markers so links keep attaching. No title/rate text.
    void RenderSnapshotNodeCollapsed(const Node& node, SnapshotCategory category, float zoom);
    /// @brief Draw the links between node pins (purely visual).
    void RenderSnapshotLinks();
    /// @brief When session.show_throughput is on, draw each visible edge's
    /// items/min at its midpoint (screen space), using pin_centers captured
    /// during the node pass and the source output pin's current_rate.
    void RenderThroughputOverlay();

    /// @brief Screen-space center of each pin's icon/marker, keyed by pin id.
    /// Filled during the node pass each frame; consumed by the throughput
    /// overlay after the editor End(). Cleared at the start of every frame.
    std::unordered_map<std::uintptr_t, ImVec2> pin_centers;
    /// @brief Record the just-drawn pin item's screen-center under `id` (called
    /// right after a pin's icon/marker is submitted).
    void RecordPinCenter(ax::NodeEditor::PinId id);

    /// @brief True when `node` should be hidden under the current session
    /// hide-* toggles (master off => always false).
    bool IsNodeHidden(const Node& node) const;
    /// @brief Recompute `visible_edges` from the model under the current hide
    /// toggles, assigning each edge a stable editor link id from NextId().
    void RebuildVisibleEdges();

    /// @brief Rebuild `hidden_production_set` from session.hidden_production_items.
    /// Call after loading a session and after any checkbox/select-all change.
    void RebuildHiddenProductionSet();

    unsigned long long int NextId();

    FactorySnapshotModel model;
    FactorySnapshotSession session;

    unsigned long long int next_id = 1;

    ax::NodeEditor::Config config;
    ax::NodeEditor::EditorContext* context = nullptr;
    /// @brief Set after a successful import; consumed on the next canvas frame to
    /// apply imported node positions and fit the view.
    bool needs_layout_apply = false;
    /// @brief Like needs_layout_apply but WITHOUT recentering the camera: applies
    /// node positions after an in-place rebuild (efficiency toggle) so the user's
    /// current pan/zoom is preserved.
    bool needs_position_apply = false;

    /// @brief The wrapper JSON of the last import, retained so an efficiency
    /// toggle can re-run the importer without re-reading the .sav. Empty until
    /// the first successful import.
    std::string retained_wrapper_json;
    /// @brief Build options of the last import; apply_efficiency is overwritten
    /// from the session on each (re)build.
    SavImport::BuildOptions retained_options;

    /// @brief Cached effective edges to draw (hidden nodes bypassed). Each entry
    /// pairs a SnapshotEdge with a stable editor link id. Rebuilt on import and
    /// whenever a hide toggle changes.
    struct CachedEdge { SnapshotEdge edge; ax::NodeEditor::LinkId id; };
    std::vector<CachedEdge> visible_edges;

    /// @brief Fast-lookup mirror of session.hidden_production_items; consulted by
    /// IsNodeHidden. Rebuilt whenever that list changes.
    std::set<std::string> hidden_production_set;

    /// @brief Unscaled text line height captured each frame before the node font
    /// scale is applied, so product-icon sizing stays independent of the font
    /// slider. Set in RenderGraphCanvas, read by the node render helpers.
    float node_base_line = 0.0f;

    /// @brief Which node set a pending jump request targets: producers of the
    /// item (Produced number / item name clicked) or consumers (Consumed number).
    enum class NavMode { Produce, Consume };

    /// @brief Pending "jump to a node for this item" request, set when a cell in
    /// the resource-flow table is clicked and consumed by RenderGraphCanvas (which
    /// runs inside the editor context, where navigation is valid). Empty = none.
    std::string nav_item;
    /// @brief Whether the pending request targets producers or consumers.
    NavMode nav_mode = NavMode::Produce;
    /// @brief Cycle bookkeeping so repeated clicks on the same item+mode step
    /// through all matching nodes; resets when the item OR the mode changes.
    std::string nav_cycle_item;
    NavMode nav_cycle_mode = NavMode::Produce;
    int nav_cycle_index = 0;

    std::string last_error;
    std::string status_text;
};
