# Factory Snapshot Zoom-Readable Nodes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the read-only Factory Snapshot graph readable when zoomed out by coloring nodes per category and collapsing them to a colored tile + a large item icon below a zoom threshold.

**Architecture:** A pure classifier `NodeSnapshotCategory` in the domain layer maps each node to one of six categories. The snapshot app maps each category to node-editor background/border colors (applied via `PushStyleColor`) and, below a zoom threshold, renders a collapsed node (big screen-constant item icon, no text, 1px pins so links stay valid) instead of the full detail node.

**Tech Stack:** C++17, Dear ImGui + imgui-node-editor, Catch2 tests, CMake/MSVC.

**Spec:** `docs/superpowers/specs/2026-06-18-snapshot-zoom-readability-design.md`

## Global Constraints

- No git commits (git-preference); record progress in `docs/superpowers/plans/2026-06-18-snapshot-zoom-readability-PROGRESS.md` instead.
- Read-only / no-balancer guarantees of the snapshot tool are unchanged.
- No new files beyond extending `node_display.{hpp,cpp}` + its test; colors/LOD live in `factory_snapshot_app.{hpp,cpp}`. No CMake changes (all touched files are already registered).
- `node_display` stays pure: no ImGui, no GL, no `Data`.
- Build (desktop): `cmake --build build --config Release --target fc-tests` (tests) / `--target ficsit-companion` (app). Tests: `ctest --test-dir build -C Release --output-on-failure`.

---

### Task 1: `NodeSnapshotCategory` classifier (pure, unit-tested)

**Files:**
- Modify: `ficsit-companion/include/domain/node_display.hpp` (add enum + decl after `NodePrimaryItem`)
- Modify: `ficsit-companion/src/domain/node_display.cpp` (add implementation)
- Test: `ficsit-companion/tests/test_node_display.cpp` (add one TEST_CASE)

**Interfaces:**
- Produces:
  - `enum class SnapshotCategory { Production, Flow, Storage, Station, Sink, Other };`
  - `SnapshotCategory NodeSnapshotCategory(const Node& node);`

- [ ] **Step 1: Write the failing test**

Append to `ficsit-companion/tests/test_node_display.cpp`:

```cpp
/// @test NodeSnapshotCategory maps each node kind (and logistics sub-kind) to its
/// coarse visual category, used to color snapshot nodes so categories are
/// distinguishable at any zoom.
/// @covers NodeSnapshotCategory for Production / Flow / Storage / Station / Sink.
TEST_CASE("NodeSnapshotCategory classifies nodes by category", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);

    ExtractorNode miner(40, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Normal, Gen);
    REQUIRE(NodeSnapshotCategory(miner) == SnapshotCategory::Production);

    MergerNode merger(41, Gen);
    REQUIRE(NodeSnapshotCategory(merger) == SnapshotCategory::Flow);

    CustomSplitterNode splitter(42, Gen);
    REQUIRE(NodeSnapshotCategory(splitter) == SnapshotCategory::Flow);

    LogisticsNode storage(43, LogisticsNode::Kind::Storage, 1, 1, Gen);
    REQUIRE(NodeSnapshotCategory(storage) == SnapshotCategory::Storage);

    LogisticsNode pipe(44, LogisticsNode::Kind::PipeJunction, 1, 1, Gen);
    REQUIRE(NodeSnapshotCategory(pipe) == SnapshotCategory::Flow);

    VehicleStationNode station(45, LogisticsNode::Kind::TrainStation, Gen);
    REQUIRE(NodeSnapshotCategory(station) == SnapshotCategory::Station);

    SinkNode sink(46, Gen);
    REQUIRE(NodeSnapshotCategory(sink) == SnapshotCategory::Sink);
}
```

- [ ] **Step 2: Add the enum + declaration to the header**

In `ficsit-companion/include/domain/node_display.hpp`, after the `NodePrimaryItem` declaration (line ~18), add:

```cpp
/// @brief Coarse visual category of a node, used by the read-only snapshot graph
/// to color nodes and pick a fallback glyph so categories stay distinguishable at
/// any zoom. Pure: no UI, no editor context.
enum class SnapshotCategory { Production, Flow, Storage, Station, Sink, Other };

/// @brief Classify a node into a SnapshotCategory by its kind (and, for logistics
/// nodes, by their logistics_kind). Pure.
SnapshotCategory NodeSnapshotCategory(const Node& node);
```

- [ ] **Step 3: Run the test to verify it fails (link/compile error)**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `NodeSnapshotCategory` unresolved / `SnapshotCategory` used in test but not yet implemented in the .cpp (the header decl alone leaves the symbol undefined at link time).

- [ ] **Step 4: Implement the classifier**

In `ficsit-companion/src/domain/node_display.cpp`, append (the file already includes `domain/node.hpp`, which defines `LogisticsNode`):

```cpp
SnapshotCategory NodeSnapshotCategory(const Node& node)
{
    switch (node.GetKind())
    {
        case Node::Kind::Craft:
        case Node::Kind::Extractor:
            return SnapshotCategory::Production;
        case Node::Kind::CustomSplitter:
        case Node::Kind::GameSplitter:
        case Node::Kind::Merger:
            return SnapshotCategory::Flow;
        case Node::Kind::Sink:
            return SnapshotCategory::Sink;
        case Node::Kind::Group:
            return SnapshotCategory::Other;
        case Node::Kind::Logistics:
        {
            const LogisticsNode& l = static_cast<const LogisticsNode&>(node);
            switch (l.logistics_kind)
            {
                case LogisticsNode::Kind::TruckStation:
                case LogisticsNode::Kind::TrainStation:
                    return SnapshotCategory::Station;
                case LogisticsNode::Kind::PipeJunction:
                    return SnapshotCategory::Flow;
                case LogisticsNode::Kind::Storage:
                case LogisticsNode::Kind::IndustrialStorage:
                case LogisticsNode::Kind::DimensionalDepot:
                case LogisticsNode::Kind::FluidBuffer:
                case LogisticsNode::Kind::IndustrialFluidBuffer:
                    return SnapshotCategory::Storage;
            }
            return SnapshotCategory::Other; // defensive: unknown future logistics kind
        }
    }
    return SnapshotCategory::Other;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build --config Release --target fc-tests` then
`ctest --test-dir build -C Release --output-on-failure -R node_display`
Expected: PASS — all `[node_display]` cases including the new one.

- [ ] **Step 6: Record progress**

Append to `docs/superpowers/plans/2026-06-18-snapshot-zoom-readability-PROGRESS.md`:
`Task 1 done — NodeSnapshotCategory classifier + unit test passing.`

---

### Task 2: Category colors + zoom level-of-detail in the snapshot canvas

**Files:**
- Modify: `ficsit-companion/include/app/factory_snapshot_app.hpp` (add two private render helpers)
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp` (color table + glyph + constants; rewrite `RenderSnapshotNode`; add detailed/collapsed helpers)

**Interfaces:**
- Consumes: `SnapshotCategory`, `NodeSnapshotCategory(const Node&)`, `NodePrimaryItem(const Node&)` (Task 1 + existing node_display).
- Produces (private methods, snapshot-internal only):
  - `void RenderSnapshotNodeDetailed(const Node& node);`
  - `void RenderSnapshotNodeCollapsed(const Node& node, SnapshotCategory category, float zoom);`

- [ ] **Step 1: Declare the two helper methods in the header**

In `ficsit-companion/include/app/factory_snapshot_app.hpp`, replace the single
`RenderSnapshotNode` declaration (line ~42-43):

```cpp
    /// @brief Draw one node: title + per-pin item & imported rate. Read-only.
    void RenderSnapshotNode(const Node& node);
```

with:

```cpp
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
```

The header already includes `domain/factory_snapshot_model.hpp`; add the
node_display include for `SnapshotCategory`. At the top of the header, after
`#include "domain/factory_snapshot_model.hpp"`, add:

```cpp
#include "domain/node_display.hpp"
```

- [ ] **Step 2: Add the color table, glyph helper, and constants**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, inside the existing
anonymous `namespace { ... }` (after `KindLabel`, before its closing `}`), add:

```cpp
    // Zoom below this collapses nodes to a colored tile + big icon (zoom < 1 is
    // zoomed out). kMaxIconScale caps how large the icon grows in node-space.
    constexpr float kCollapseZoom = 0.5f;
    constexpr float kMaxIconScale = 5.0f;

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
```

- [ ] **Step 3: Rewrite `RenderSnapshotNode` to push colors + branch on zoom**

Replace the current `RenderSnapshotNode` definition (the whole function, ~lines
329-394) with the dispatcher below, then the two helpers in Steps 4 and 5:

```cpp
void FactorySnapshotApp::RenderSnapshotNode(const Node& node)
{
    const SnapshotCategory category = NodeSnapshotCategory(node);
    const NodeColors colors = CategoryColors(category);
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBg, colors.bg);
    ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBorder, colors.border);

    ax::NodeEditor::BeginNode(node.id);

    const float zoom = ax::NodeEditor::GetCurrentZoom();
    if (zoom < kCollapseZoom)
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
```

- [ ] **Step 4: Add `RenderSnapshotNodeDetailed` (the former node body)**

Add immediately after `RenderSnapshotNode`. This is the previous full-detail body
moved verbatim, minus the `BeginNode`/`EndNode` calls (the dispatcher owns them):

```cpp
void FactorySnapshotApp::RenderSnapshotNodeDetailed(const Node& node)
{
    // Header: a prominent "product" icon (the node's representative item) that
    // grows in node-space as the view zooms out, so productions stay recognizable
    // from afar; the title sits beside it.
    const Item* product = NodePrimaryItem(node);
    if (product != nullptr && product->icon_gl_index != 0)
    {
        const float zoom = ax::NodeEditor::GetCurrentZoom();
        const float base = ImGui::GetTextLineHeightWithSpacing();
        const float icon_size = base * std::clamp(1.0f / (zoom > 0.0f ? zoom : 1.0f), 1.0f, 3.0f);
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
```

- [ ] **Step 5: Add `RenderSnapshotNodeCollapsed`**

Add immediately after `RenderSnapshotNodeDetailed`:

```cpp
void FactorySnapshotApp::RenderSnapshotNodeCollapsed(const Node& node, SnapshotCategory category, float zoom)
{
    // Inputs: submit each pin as a 1px marker (no label) so links keep attaching.
    ImGui::BeginGroup();
    for (const auto& pin : node.ins)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();
    ImGui::SameLine();

    // Body: a large, roughly screen-constant item icon, or a category glyph when
    // the node has no item icon.
    const float base = ImGui::GetTextLineHeightWithSpacing();
    const float size = base * std::clamp(1.0f / (zoom > 0.0f ? zoom : 1.0f), 1.0f, kMaxIconScale);
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

    // Outputs: 1px markers on the right.
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
}
```

- [ ] **Step 6: Build the app**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: builds clean (no warnings/errors in `factory_snapshot_app.cpp`).

- [ ] **Step 7: Manual smoke test (GUI)**

Run the app, load a save into the Factory Snapshot tab, then:
- Zoom out: nodes shrink to colored tiles (blue=production, orange=splitter/merger, green=storage, purple=station, red=sink) each showing a large item icon; categories are tellable apart; links still connect tile-to-tile.
- Zoom in: full detail returns (titles, per-pin icons, rates); vehicle routes still link station-to-station.
- If colors are too strong/weak or the collapse happens too early/late, tune the `Rgba(...)` values / `kCollapseZoom` / `kMaxIconScale` constants and rebuild.

- [ ] **Step 8: Record progress**

Append to `docs/superpowers/plans/2026-06-18-snapshot-zoom-readability-PROGRESS.md`:
`Task 2 done — category colors + zoom LOD in snapshot canvas; app builds; smoke-tested.`

---

## Self-Review

- **Spec coverage:** §1 classifier → Task 1. §2 colors (PushStyleColor, medium palette) → Task 2 Steps 2-3. §3 LOD (collapse threshold, screen-constant icon, 1px pins, plug, no-item glyph) → Task 2 Steps 2,3,5. Detailed path preserved → Task 2 Step 4. Testing (unit + manual smoke) → Task 1 Step 5, Task 2 Step 7. Non-goals respected (no link styling, no new files, no commits).
- **Placeholder scan:** none — every code/command step is concrete.
- **Type consistency:** `SnapshotCategory` enum values (Production/Flow/Storage/Station/Sink/Other) used identically in classifier, color table, glyph, and dispatcher. Helper signatures match between header (Step 1) and definitions (Steps 4-5). `NodePrimaryItem` / `NodeDisplayName` / `DrawPinIcon` / `DrawPinMarker` / `PinLabel` are existing symbols reused unchanged.
- **Note on Group:** `Group → Other` is exercised via the default branch, not a dedicated unit assertion (constructing a `GroupNode` needs sub-nodes/links; out of scope for the pure test). Acceptable per spec's "Other (and any unmapped kind)".
