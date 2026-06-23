#pragma once

#include <string>

#include <imgui_node_editor.h>

#include "app/base_app.hpp"
#include "domain/snapshot/factory_snapshot_model.hpp"
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

    unsigned long long int NextId();

    FactorySnapshotModel model;
    FactorySnapshotSession session;

    unsigned long long int next_id = 1;

    ax::NodeEditor::Config config;
    ax::NodeEditor::EditorContext* context = nullptr;
    /// @brief Set after a successful import; consumed on the next canvas frame to
    /// apply imported node positions and fit the view.
    bool needs_layout_apply = false;

    /// @brief Unscaled text line height captured each frame before the node font
    /// scale is applied, so product-icon sizing stays independent of the font
    /// slider. Set in RenderGraphCanvas, read by the node render helpers.
    float node_base_line = 0.0f;

    /// @brief Pending "jump to a producer of this item" request, set when a row in
    /// the resource-flow table is clicked and consumed by RenderGraphCanvas (which
    /// runs inside the editor context, where navigation is valid). Empty = none.
    std::string nav_item;
    /// @brief Cycle bookkeeping so repeated clicks on the same item step through
    /// all of its producers; resets when a different item is clicked.
    std::string nav_cycle_item;
    int nav_cycle_index = 0;

    std::string last_error;
    std::string status_text;
};
