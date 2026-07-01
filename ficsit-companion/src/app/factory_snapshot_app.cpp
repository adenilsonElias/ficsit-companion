#include "app/factory_snapshot_app.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>

#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#endif

#include "domain/graph/link.hpp"          // full definition for unique_ptr<Link> destruction
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node_display.hpp"
#include "domain/gamedata/recipe.hpp"        // Item::name
#include "domain/snapshot/resource_flow.hpp"
#include "infra/saveimport/factory_snapshot_builder.hpp"

namespace
{
    constexpr const char* kSessionFile = "saved/factory_snapshot.json";

    // A small connection marker; the node editor attaches links to the pin rect.
    void DrawPinMarker()
    {
        const float r = 4.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + r), r, IM_COL32(200, 200, 200, 255));
    }

    // A pin's connection point: the item's icon when available, else the marker.
    // The drawn element is the pin rect the node editor attaches links to.
    void DrawPinIcon(const Item* item)
    {
        if (item != nullptr && item->icon_gl_index != 0)
        {
            const float h = ImGui::GetTextLineHeightWithSpacing();
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(item->icon_gl_index)), ImVec2(h, h));
        }
        else
        {
            DrawPinMarker();
        }
    }

    // "<item>  <rate>/min" using the imported rate (no balancing). Shows the
    // decimal value (like the modeler) rather than the exact fraction: splitter
    // division produces rates such as 100/3, and "100/3/min" reads as a broken
    // value, whereas "33.333/min" matches what the planner displays.
    // Takes by non-const ref because FractionalNumber::GetStringFloat() is not const
    // (it lazily caches the string representation).
    std::string PinLabel(Pin& pin)
    {
        const std::string item = pin.item ? pin.item->name : std::string("(none)");
        return item + "  " + pin.current_rate.GetStringFloat() + "/min";
    }

    // Rate value only (no item name), used to flank the resource icon in the
    // collapsed/zoomed-out view: input values on the left, output on the right.
    std::string PinRate(Pin& pin)
    {
        return pin.current_rate.GetStringFloat() + "/min";
    }

    // Sum of every pin's rate in a list, as a single "/min" string. Used in the
    // collapsed/zoomed-out view to show one aggregated number per side.
    template <typename PinList>
    std::string TotalRate(const PinList& pins)
    {
        FractionalNumber total{ 0, 1 };
        for (const auto& pin : pins)
            total += pin->current_rate;
        return total.GetStringFloat() + "/min";
    }

    const char* KindLabel(Node::Kind kind)
    {
        switch (kind)
        {
            case Node::Kind::Craft:          return "Machines";
            case Node::Kind::CustomSplitter: return "Custom Splitters";
            case Node::Kind::Merger:         return "Mergers";
            case Node::Kind::Group:          return "Groups";
            case Node::Kind::GameSplitter:   return "Splitters";
            case Node::Kind::Sink:           return "Sinks";
            case Node::Kind::Extractor:      return "Extractors";
            case Node::Kind::Logistics:      return "Logistics";
        }
        return "Other";
    }

    // ax::NodeEditor::GetCurrentZoom() returns the view's InvScale: it is LARGE
    // when zoomed OUT (e.g. 2 == half size, 5 == one-fifth size) and < 1 when
    // zoomed in. Above kCollapseZoom (zoomed out past ~2x) nodes collapse to a
    // colored tile + big icon. To stay a roughly constant size on screen, the
    // node-space icon must GROW with this value (size = target * clamp(zoom, 1,
    // kMaxIconScale)); kMaxIconScale caps node-space growth when very far out.
    // kCollapsedScreenScale is the collapsed icon's target on-screen size in
    // multiples of a text line, so the tiles stay big and readable when zoomed out.
    constexpr float kCollapseZoom        = 2.0f;
    constexpr float kMaxIconScale        = 12.0f;
    constexpr float kCollapsedScreenScale = 3.5f;

    struct NodeColors { ImVec4 bg; ImVec4 border; };

    ImVec4 Rgba(int r, int g, int b, int a)
    {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    }

    // Medium-saturation fill + brighter border per category, so categories are
    // distinguishable at any zoom (the fill is the node's real background).
    NodeColors CategoryColors(SnapshotCategory cat)
    {
        switch (cat)
        {
            case SnapshotCategory::Production: return { Rgba(60,110,200,200),  Rgba(120,170,255,255) };
            case SnapshotCategory::Flow:       return { Rgba(200,130,40,200),  Rgba(255,190,90,255) };
            case SnapshotCategory::Storage:    return { Rgba(55,160,90,200),   Rgba(110,220,150,255) };
            case SnapshotCategory::Station:    return { Rgba(150,90,200,200),  Rgba(200,150,255,255) };
            case SnapshotCategory::Sink:       return { Rgba(200,70,70,200),   Rgba(255,130,130,255) };
            case SnapshotCategory::Other:      return { Rgba(110,110,120,200), Rgba(170,170,180,255) };
        }
        return { Rgba(110,110,120,200), Rgba(170,170,180,255) };
    }

    // Single-letter fallback when a node has no item icon to show when collapsed.
    char CategoryGlyph(SnapshotCategory cat)
    {
        switch (cat)
        {
            case SnapshotCategory::Production: return 'P';
            case SnapshotCategory::Flow:       return 'F';
            case SnapshotCategory::Storage:    return 'S';
            case SnapshotCategory::Station:    return 'T';
            case SnapshotCategory::Sink:       return 'K';
            case SnapshotCategory::Other:      return '-';
        }
        return '-';
    }
}

FactorySnapshotApp::FactorySnapshotApp()
{
    LoadSession();
    config.SettingsFile = nullptr;     // no on-disk layout file
    config.EnableSmoothZoom = true;
    // Triple the zoom-out range vs. the editor default (which bottoms out at
    // 0.1 scale) by prepending three further-out steps down to ~0.033 scale.
    // CustomZoomLevels is referenced by pointer by the editor, so it must be
    // populated before CreateEditor and outlive the context (it is a member).
    for (const float level : { 0.033f, 0.05f, 0.075f,
                               0.1f, 0.15f, 0.20f, 0.25f, 0.33f, 0.5f, 0.75f,
                               1.0f, 1.25f, 1.50f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f })
        config.CustomZoomLevels.push_back(level);
    context = ax::NodeEditor::CreateEditor(&config);
}

FactorySnapshotApp::~FactorySnapshotApp()
{
    ax::NodeEditor::DestroyEditor(context);
}

unsigned long long int FactorySnapshotApp::NextId()
{
    return next_id++;
}

void FactorySnapshotApp::LoadSession()
{
#if !defined(__EMSCRIPTEN__)
    std::ifstream f(kSessionFile, std::ios::binary);
    if (f)
    {
        std::ostringstream ss;
        ss << f.rdbuf();
        session.Deserialize(ss.str());
    }
#endif
    RebuildHiddenProductionSet();
}

void FactorySnapshotApp::SaveSession()
{
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::create_directories("saved", ec);
    std::ofstream f(kSessionFile, std::ios::binary | std::ios::trunc);
    if (f) f << session.Serialize();
#endif
}

void FactorySnapshotApp::LoadFromWrapperJson(const std::string& wrapper_json,
                                             const SavImport::BuildOptions& options)
{
    last_error.clear();

    std::string err;
    if (!FactorySnapshot::BuildSnapshotFromJson(wrapper_json,
            [this] { return NextId(); }, options, model, err))
    {
        last_error = err;
        status_text.clear();
        return;
    }

    std::ostringstream st;
    st << model.nodes.size() << " buildings, " << model.flow.rows.size() << " item flows, "
       << model.warnings.size() << " warning(s)";
    status_text = st.str();
    needs_layout_apply = true; // apply imported node positions + fit on next canvas frame
}

void FactorySnapshotApp::RenderImpl()
{
    RenderStatusLine();
    ImGui::Separator();

    ImGui::BeginChild("##snapshot_left", ImVec2(340.0f, 0.0f), true);
    if (ImGui::BeginTabBar("##snapshot_left_tabs"))
    {
        if (ImGui::BeginTabItem("Resources"))
        {
            RenderResourceFlowTable();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info"))
        {
            RenderTopologySummary();
            ImGui::Separator();
            RenderWarningsPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Options"))
        {
            RenderOptionsPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (model.nodes.empty())
    {
        ImGui::BeginChild("##snapshot_graph_empty", ImVec2(0.0f, 0.0f), true);
        ImGui::TextDisabled("Load a save from the bar above to populate this view.");
        ImGui::EndChild();
    }
    else
    {
        RenderGraphCanvas();
    }
}

void FactorySnapshotApp::RenderStatusLine()
{
    ImGui::TextUnformatted("Factory Snapshot (read-only)");
    ImGui::SameLine();
    if (!last_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "  %s", last_error.c_str());
    }
    else if (!status_text.empty())
    {
        ImGui::TextDisabled("  %s", status_text.c_str());
    }
    else
    {
        ImGui::TextDisabled("  Load a save from the bar above to populate this view.");
    }
}

void FactorySnapshotApp::RenderTopologySummary()
{
    ImGui::TextUnformatted("Topology");
    if (model.nodes.empty())
    {
        ImGui::TextDisabled("(nothing imported)");
        return;
    }
    for (int k = 0; k <= static_cast<int>(Node::Kind::Logistics); ++k)
    {
        const std::size_t count = model.CountOfKind(k);
        if (count == 0) continue;
        ImGui::BulletText("%s: %zu", KindLabel(static_cast<Node::Kind>(k)), count);
    }
}

void FactorySnapshotApp::RenderWarningsPanel()
{
    ImGui::Text("Warnings (%zu)", model.warnings.size());
    if (model.warnings.empty())
    {
        ImGui::TextDisabled("(none)");
        return;
    }
    for (const std::string& w : model.warnings)
    {
        ImGui::TextWrapped("- %s", w.c_str());
    }
}

void FactorySnapshotApp::RenderOptionsPanel()
{
    ImGui::TextUnformatted("Graph view");
    ImGui::Spacing();

    ImGui::TextUnformatted("Box font size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool changed = ImGui::SliderFloat("##snapshot_font_scale", &session.node_font_scale, 0.5f, 3.0f, "%.2fx");

    ImGui::Spacing();
    ImGui::TextUnformatted("Production icon size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat("##snapshot_icon_scale", &session.icon_scale, 0.5f, 4.0f, "%.2fx");

    ImGui::Spacing();
    ImGui::TextUnformatted("Zoom-out number font size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat("##snapshot_collapsed_font_scale", &session.collapsed_font_scale, 0.5f, 3.0f, "%.2fx");

    if (changed) SaveSession();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Declutter (visualization only)");

    bool hide_changed = ImGui::Checkbox("Hide splitters / mergers / logistics",
                                        &session.hide_logistics_enabled);

    ImGui::BeginDisabled(!session.hide_logistics_enabled);
    ImGui::Indent();
    hide_changed |= ImGui::Checkbox("Game Splitters", &session.hide_game_splitters);
    hide_changed |= ImGui::Checkbox("Custom Splitters", &session.hide_custom_splitters);
    hide_changed |= ImGui::Checkbox("Mergers", &session.hide_mergers);
    hide_changed |= ImGui::Checkbox("Logistics", &session.hide_logistics_nodes);
    ImGui::Unindent();
    ImGui::EndDisabled();

    if (hide_changed)
    {
        SaveSession();
        RebuildVisibleEdges();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults"))
    {
        session.node_font_scale = 1.0f;
        session.icon_scale = 1.0f;
        session.collapsed_font_scale = 1.0f;
        session.hide_logistics_enabled = false;
        session.hide_game_splitters = true;
        session.hide_custom_splitters = true;
        session.hide_mergers = true;
        session.hide_logistics_nodes = true;
        RebuildVisibleEdges();
        SaveSession();
    }
}

void FactorySnapshotApp::RenderResourceFlowTable()
{
    ImGui::TextUnformatted("Resource flow");

    int filter = session.flow_filter;
    ImGui::RadioButton("All", &filter, 0); ImGui::SameLine();
    ImGui::RadioButton("Deficit", &filter, 1); ImGui::SameLine();
    ImGui::RadioButton("Surplus", &filter, 2);
    if (filter != session.flow_filter) { session.flow_filter = filter; SaveSession(); }

    if (ImGui::InputText("Search##flow", &session.flow_search))
    {
        SaveSession();
    }

    if (ImGui::SmallButton("Select all##prod"))
    {
        session.hidden_production_items.clear();
        RebuildHiddenProductionSet();
        RebuildVisibleEdges();
        SaveSession();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Unselect all##prod"))
    {
        // Hide every producible item currently in the report.
        std::set<std::string> all;
        for (ResourceFlowRow& r : model.flow.rows)
            if (r.item && r.produced != FractionalNumber(0, 1))
                all.insert(r.item->name);
        session.hidden_production_items.assign(all.begin(), all.end());
        RebuildHiddenProductionSet();
        RebuildVisibleEdges();
        SaveSession();
    }

    const ResourceFlowFilter flow_filter = static_cast<ResourceFlowFilter>(session.flow_filter);

    // Fill the rest of the tab so the table uses the whole left panel.
    if (ImGui::BeginTable("##flow_table", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
        ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Produced");
        ImGui::TableSetupColumn("Consumed");
        ImGui::TableSetupColumn("Net");
        ImGui::TableHeadersRow();

        for (ResourceFlowRow& row : model.flow.rows)
        {
            if (!RowPassesFilter(row, flow_filter, session.flow_search)) continue;
            ImGui::TableNextRow();
            // Cell text (item names, rate strings) repeats across rows, so scope
            // the Selectable ids by the row's item pointer to keep them unique.
            ImGui::PushID(static_cast<const void*>(row.item));

            ImGui::TableNextColumn();
            // Per-item visibility checkbox. Only produced items have producers to
            // hide; consumed-only rows leave the cell empty. Checked == visible
            // (item NOT in the hidden set).
            if (row.item && row.produced != FractionalNumber(0, 1))
            {
                bool visible = hidden_production_set.find(row.item->name) == hidden_production_set.end();
                if (ImGui::Checkbox("##show", &visible))
                {
                    if (visible)
                    {
                        auto& v = session.hidden_production_items;
                        v.erase(std::remove(v.begin(), v.end(), row.item->name), v.end());
                    }
                    else
                    {
                        session.hidden_production_items.push_back(row.item->name);
                    }
                    RebuildHiddenProductionSet();
                    RebuildVisibleEdges();
                    SaveSession();
                }
            }

            ImGui::TableNextColumn();
            // Click a cell to jump the graph to a node for this item; navigation
            // itself runs in RenderGraphCanvas (editor context). The item name and
            // the Produced number jump to producers, the Consumed number to
            // consumers. The "##name"/"##prod"/"##cons" suffixes hide a per-cell id
            // so identical displayed text (e.g. Produced and Consumed both "0") in
            // the same row does not collide.
            const std::string item_name = row.item ? row.item->name : std::string("(unknown)");
            if (ImGui::Selectable((item_name + "##name").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Produce;
            }

            ImGui::TableNextColumn();
            if (ImGui::Selectable((row.produced.GetStringFloat() + "##prod").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Produce;
            }

            ImGui::TableNextColumn();
            if (ImGui::Selectable((row.consumed.GetStringFloat() + "##cons").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Consume;
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.net.GetStringFloat().c_str());

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void FactorySnapshotApp::RenderGraphCanvas()
{
    ax::NodeEditor::SetCurrentEditor(context);

    if (needs_layout_apply)
    {
        for (const auto& node : model.nodes)
        {
            ax::NodeEditor::SetNodePosition(node->id, node->pos);
        }
        RebuildVisibleEdges();
    }

    // Capture the unscaled line height before applying the font scale, so the
    // product-icon slider stays independent of the font slider.
    node_base_line = ImGui::GetTextLineHeightWithSpacing();

    ax::NodeEditor::Begin("##snapshot_graph", ImGui::GetContentRegionAvail());

    // Scale only the snapshot node text (Options > Box font size). Reset after
    // the node pass so nothing else in this window is affected.
    ImGui::SetWindowFontScale(session.node_font_scale);
    for (const auto& node : model.nodes)
    {
        if (IsNodeHidden(*node)) continue;
        RenderSnapshotNode(*node);
    }
    ImGui::SetWindowFontScale(1.0f);
    RenderSnapshotLinks();

    ax::NodeEditor::End();

    if (needs_layout_apply)
    {
        ax::NodeEditor::NavigateToContent();
        needs_layout_apply = false;
    }

    // Consume a pending "jump to a node for this item" request from the
    // resource-flow table. Repeated clicks on the same item+mode cycle through
    // all matching nodes (producers for Produce, consumers for Consume).
    if (!nav_item.empty())
    {
        const std::vector<const Node*> targets =
            nav_mode == NavMode::Produce
                ? NodesProducingItem(model.nodes, nav_item)
                : NodesConsumingItem(model.nodes, nav_item);
        if (!targets.empty())
        {
            if (nav_item != nav_cycle_item || nav_mode != nav_cycle_mode)
            {
                nav_cycle_item = nav_item;
                nav_cycle_mode = nav_mode;
                nav_cycle_index = 0;
            }
            const Node* target = targets[nav_cycle_index % targets.size()];
            ax::NodeEditor::SelectNode(target->id);
            ax::NodeEditor::NavigateToSelection();
            nav_cycle_index = (nav_cycle_index + 1) % static_cast<int>(targets.size());
        }
        nav_item.clear();
    }

    ax::NodeEditor::SetCurrentEditor(nullptr);
}

void FactorySnapshotApp::RenderSnapshotNode(const Node& node)
{
    const SnapshotCategory category = NodeSnapshotCategory(node);
    const NodeColors colors = CategoryColors(category);
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBg, colors.bg);
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBorder, colors.border);

    ax::NodeEditor::BeginNode(node.id);

    const float zoom = ax::NodeEditor::GetCurrentZoom();
    if (zoom > kCollapseZoom)
    {
        RenderSnapshotNodeCollapsed(node, category, zoom);
    }
    else
    {
        RenderSnapshotNodeDetailed(node);
    }

    ax::NodeEditor::EndNode();
    ax::NodeEditor::PopStyleColor(2);
}

void FactorySnapshotApp::RenderSnapshotNodeDetailed(const Node& node)
{
    // Header: a prominent "product" icon (the node's representative item) that
    // grows in node-space as the view zooms out, so productions stay recognizable
    // from afar; the title sits beside it.
    const Item* product = NodePrimaryItem(node);
    if (product != nullptr && product->icon_gl_index != 0)
    {
        const float zoom = ax::NodeEditor::GetCurrentZoom();
        const float icon_size = node_base_line * session.icon_scale * std::clamp(zoom, 1.0f, 3.0f);
        ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(product->icon_gl_index)), ImVec2(icon_size, icon_size));
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(NodeDisplayName(node).c_str());

    ImGui::BeginGroup(); // inputs: icon then label
    for (const auto& pin : node.ins)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        DrawPinIcon(pin->item);
        ImGui::SameLine();
        ImGui::TextUnformatted(PinLabel(*pin).c_str());
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup(); // outputs: label then icon
    for (const auto& pin : node.outs)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        ImGui::TextUnformatted(PinLabel(*pin).c_str());
        ImGui::SameLine();
        DrawPinIcon(pin->item);
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();

    // Vehicle station plug (held outside ins/outs) so plug<->plug route links resolve.
    if (node.IsLogistics())
    {
        const LogisticsNode& lg = static_cast<const LogisticsNode&>(node);
        if (lg.logistics_kind == LogisticsNode::Kind::TruckStation ||
            lg.logistics_kind == LogisticsNode::Kind::TrainStation)
        {
            const VehicleStationNode& v = static_cast<const VehicleStationNode&>(lg);
            if (v.plug)
            {
                ImGui::SameLine();
                ImGui::BeginGroup();
                ax::NodeEditor::BeginPin(v.plug->id, v.plug->direction);
                DrawPinMarker();
                ImGui::SameLine();
                ImGui::TextUnformatted("Route");
                ax::NodeEditor::EndPin();
                ImGui::EndGroup();
            }
        }
    }
}

void FactorySnapshotApp::RenderSnapshotNodeCollapsed(const Node& node, SnapshotCategory category, float zoom)
{
    // Text in node-space shrinks on screen as the view zooms out, so grow the font
    // with the same factor used for the collapsed icon to keep the rate values
    // readable. The collapsed numbers have their own Options > "Zoom-out number font
    // size" (session.collapsed_font_scale), independent of the detailed-view "Box font
    // size". Restore node_font_scale (applied by the node pass) on exit so following
    // detailed nodes keep that setting.
    const float text_scale = session.collapsed_font_scale * std::clamp(zoom, 1.0f, kMaxIconScale);
    ImGui::SetWindowFontScale(text_scale);

    // Inputs: one aggregated rate value on the LEFT of the resource icon. Each pin is
    // still submitted as a 1px marker (stacked at the group's left edge) so links keep
    // attaching, but only the summed total is shown.
    ImGui::BeginGroup();
    for (const auto& pin : node.ins)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();
    if (!node.ins.empty())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(TotalRate(node.ins).c_str());
    }
    ImGui::SameLine();

    // Body: a large, roughly screen-constant item icon, or a category glyph when
    // the node has no item icon.
    const float size = node_base_line * kCollapsedScreenScale * session.icon_scale * std::clamp(zoom, 1.0f, kMaxIconScale);
    const Item* product = NodePrimaryItem(node);
    if (product != nullptr && product->icon_gl_index != 0)
    {
        ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(product->icon_gl_index)), ImVec2(size, size));
    }
    else
    {
        const char glyph[2] = { CategoryGlyph(category), '\0' };
        ImGui::Dummy(ImVec2(size, size));
        const ImVec2 box_min = ImGui::GetItemRectMin();
        const ImVec2 box_max = ImGui::GetItemRectMax();
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2((box_min.x + box_max.x - ts.x) * 0.5f, (box_min.y + box_max.y - ts.y) * 0.5f),
            IM_COL32(255, 255, 255, 255), glyph);
    }

    ImGui::SameLine();

    // Outputs: one aggregated rate value on the RIGHT of the resource icon. Each pin is
    // still submitted as a 1px marker (stacked at the group's right edge) so links keep
    // attaching, but only the summed total is shown.
    if (!node.outs.empty())
    {
        ImGui::TextUnformatted(TotalRate(node.outs).c_str());
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    for (const auto& pin : node.outs)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();

    // Vehicle-station plug stays submitted so route (plug<->plug) links stay valid.
    if (node.IsLogistics())
    {
        const LogisticsNode& lg = static_cast<const LogisticsNode&>(node);
        if (lg.logistics_kind == LogisticsNode::Kind::TruckStation ||
            lg.logistics_kind == LogisticsNode::Kind::TrainStation)
        {
            const VehicleStationNode& v = static_cast<const VehicleStationNode&>(lg);
            if (v.plug)
            {
                ImGui::SameLine();
                ax::NodeEditor::BeginPin(v.plug->id, v.plug->direction);
                ImGui::Dummy(ImVec2(1.0f, 1.0f));
                ax::NodeEditor::EndPin();
            }
        }
    }

    ImGui::SetWindowFontScale(session.node_font_scale);
}

void FactorySnapshotApp::RebuildHiddenProductionSet()
{
    hidden_production_set.clear();
    hidden_production_set.insert(session.hidden_production_items.begin(),
                                session.hidden_production_items.end());
}

bool FactorySnapshotApp::IsNodeHidden(const Node& node) const
{
    if (NodeProductionHidden(node, hidden_production_set)) return true;
    if (!session.hide_logistics_enabled) return false;
    switch (node.GetKind())
    {
        case Node::Kind::GameSplitter:   return session.hide_game_splitters;
        case Node::Kind::CustomSplitter: return session.hide_custom_splitters;
        case Node::Kind::Merger:         return session.hide_mergers;
        case Node::Kind::Logistics:      return session.hide_logistics_nodes;
        default:                         return false;
    }
}

void FactorySnapshotApp::RebuildVisibleEdges()
{
    // Logistics-hidden nodes reroute (bypass); production-hidden nodes are
    // dropped (their links vanish). should_bypass = "not a production hide".
    auto should_bypass = [this](const Node& n) {
        return !NodeProductionHidden(n, hidden_production_set);
    };
    visible_edges.clear();
    const std::vector<SnapshotEdge> edges = ComputeVisibleEdges(
        model.nodes, model.links,
        [this](const Node& n) { return IsNodeHidden(n); },
        should_bypass);
    visible_edges.reserve(edges.size());
    for (const SnapshotEdge& e : edges)
        visible_edges.push_back({ e, ax::NodeEditor::LinkId(NextId()) });
}

void FactorySnapshotApp::RenderSnapshotLinks()
{
    for (const CachedEdge& ce : visible_edges)
    {
        ax::NodeEditor::Link(ce.id, ce.edge.start_id, ce.edge.end_id);
    }
}
