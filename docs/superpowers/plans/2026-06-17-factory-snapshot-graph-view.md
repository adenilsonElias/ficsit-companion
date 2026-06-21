# Factory Snapshot Read-only Node Graph — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the imported factory in the Factory Snapshot tab as a modeler-style node graph that is read-only (no editing) and shows rates exactly as imported (no balancer).

**Architecture:** `FactorySnapshotApp` hosts its own `imgui-node-editor` context and draws the `model.nodes`/`model.links` it already owns. A small pure `NodeDisplayName()` domain helper supplies node titles. No edit/create/delete handlers are wired and `UpdateNodesRate` is never called, so the view is structurally read-only.

**Tech Stack:** C++17, Dear ImGui + imgui-node-editor, Catch2 tests, CMake (Visual Studio generator). Spec: `docs/superpowers/specs/2026-06-17-factory-snapshot-graph-view-design.md`.

## Global Constraints

- **No git commits** (per user git-preference). Track progress in the spec/PROGRESS notes, not via commits. Each task ends by building + running the test suite and ticking its checkbox.
- C++17. Follow the layered layout: `domain/` is pure logic (no UI/IO, may not depend on `infra`/`app`); `app/` may depend on both.
- Build (tests): `cmake --build build --config Release --target fc-tests`
- Run tests: `ctest --test-dir build -C Release --output-on-failure` (must stay green: 130 passing before this plan).
- Build (GUI): `cmake --build build --config Release --target ficsit-companion`
- Rates are displayed via `FractionalNumber::GetStringFraction()`; never run rate propagation.

---

### Task 1: `NodeDisplayName()` domain helper

A pure function mapping a `Node` to its human title, reused by the snapshot renderer and unit-testable without an editor context.

**Files:**
- Create: `ficsit-companion/include/domain/node_display.hpp`
- Create: `ficsit-companion/src/domain/node_display.cpp`
- Test: `ficsit-companion/tests/test_node_display.cpp`
- Modify: `ficsit-companion/CMakeLists.txt` (add source, header, test to their lists)

**Interfaces:**
- Produces: `std::string NodeDisplayName(const Node& node);` — title text per `Node::Kind`.

- [ ] **Step 1: Register the new files with CMake**

In `ficsit-companion/CMakeLists.txt`, add the source to `DOMAIN_SOURCE_FILES` (after `src/domain/node_data_resolver.cpp`):

```cmake
    src/domain/node_data_resolver.cpp
    src/domain/node_display.cpp
```

Add the header to `HEADER_FILES` (after `include/domain/node_data_resolver.hpp`):

```cmake
	include/domain/node_data_resolver.hpp
	include/domain/node_display.hpp
```

Add the test to `TEST_SOURCE_FILES` (after `tests/test_resource_flow.cpp`):

```cmake
        tests/test_resource_flow.cpp
        tests/test_node_display.cpp
```

- [ ] **Step 2: Write the failing test**

Create `ficsit-companion/tests/test_node_display.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "domain/node.hpp"
#include "domain/node_display.hpp"

namespace
{
    unsigned long long g_counter = 1;
    unsigned long long Gen() { return g_counter++; }
}

/// @test NodeDisplayName labels the organizer and sink kinds with fixed names
/// (these node kinds construct without game data or an editor context).
/// @covers NodeDisplayName for Merger / CustomSplitter / GameSplitter / Sink.
TEST_CASE("NodeDisplayName labels organizer and sink kinds", "[node_display]")
{
    MergerNode merger(1, Gen);
    REQUIRE(NodeDisplayName(merger) == "Merger");

    CustomSplitterNode custom_splitter(2, Gen);
    REQUIRE(NodeDisplayName(custom_splitter) == "Splitter");

    GameSplitterNode game_splitter(3, Gen);
    REQUIRE(NodeDisplayName(game_splitter) == "Splitter");

    SinkNode sink(4, Gen);
    REQUIRE(NodeDisplayName(sink) == "AWESOME Sink");
}
```

- [ ] **Step 3: Run the test to verify it fails to compile/link**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `node_display.hpp` not found / `NodeDisplayName` unresolved.

- [ ] **Step 4: Create the header**

Create `ficsit-companion/include/domain/node_display.hpp`:

```cpp
#pragma once

#include <string>

struct Node;

/// @brief Human-readable title for a node, by kind: recipe / extractor building
/// (+resource) / "Merger" / "Splitter" / "AWESOME Sink" / logistics display name /
/// group name. Pure: no UI, no editor context. Used by the read-only snapshot graph.
std::string NodeDisplayName(const Node& node);
```

- [ ] **Step 5: Create the implementation**

Create `ficsit-companion/src/domain/node_display.cpp`:

```cpp
#include "domain/node_display.hpp"

#include "domain/node.hpp"
#include "domain/recipe.hpp"   // Recipe::display_name, Item::name
#include "domain/building.hpp" // Building::name

std::string NodeDisplayName(const Node& node)
{
    switch (node.GetKind())
    {
        case Node::Kind::Craft:
        {
            const CraftNode& c = static_cast<const CraftNode&>(node);
            return c.recipe ? c.recipe->display_name : std::string("Recipe");
        }
        case Node::Kind::Extractor:
        {
            const ExtractorNode& e = static_cast<const ExtractorNode&>(node);
            const Building* b = e.GetBuilding();
            std::string title = b ? b->name : std::string("Extractor");
            if (e.resource) title += " (" + e.resource->name + ")";
            return title;
        }
        case Node::Kind::Merger:         return "Merger";
        case Node::Kind::CustomSplitter: return "Splitter";
        case Node::Kind::GameSplitter:   return "Splitter";
        case Node::Kind::Sink:           return "AWESOME Sink";
        case Node::Kind::Logistics:
        {
            const LogisticsNode& l = static_cast<const LogisticsNode&>(node);
            return l.GetDisplayName();
        }
        case Node::Kind::Group:
        {
            const GroupNode& g = static_cast<const GroupNode&>(node);
            return g.name.empty() ? std::string("Group") : g.name;
        }
    }
    return "Node";
}
```

- [ ] **Step 6: Reconfigure CMake (new files were added to the lists)**

Run: `cmake -S . -B build` (re-globs the explicit lists into the VS project)
Expected: configures with no errors.

- [ ] **Step 7: Build and run the test**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS — the new `[node_display]` test passes; total now 131 tests, 0 failed.

- [ ] **Step 8: No commit — tick this task in the PROGRESS notes (per git-preference).**

---

### Task 2: Editor context + read-only graph rendering methods

Give `FactorySnapshotApp` its own node-editor context and the three private methods that draw the graph. Not yet called from `RenderImpl` (wired in Task 3), so this task compiles cleanly but does not change the visible UI.

**Files:**
- Modify: `ficsit-companion/include/app/factory_snapshot_app.hpp`
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp`

**Interfaces:**
- Consumes: `NodeDisplayName(const Node&)` from Task 1; `model.nodes`/`model.links` (already on the app); `Pin::item`, `Pin::current_rate`, `Pin::direction`, `Pin::id`; `Link::id`, `Link::start_id`, `Link::end_id`.
- Produces: private methods `void RenderGraphCanvas();`, `void RenderSnapshotNode(const Node& node);`, `void RenderSnapshotLinks();`; members `ax::NodeEditor::Config config;`, `ax::NodeEditor::EditorContext* context;`, `bool needs_layout_apply;`.

- [ ] **Step 1: Add the node-editor include and members to the header**

In `ficsit-companion/include/app/factory_snapshot_app.hpp`, add the include after the existing includes block:

```cpp
#include <imgui_node_editor.h>

#include "app/base_app.hpp"
#include "domain/factory_snapshot_model.hpp"
#include "infra/factory_snapshot_session.hpp"
```

Add the method declarations in the `private:` section, after `void RenderResourceFlowTable();`:

```cpp
    void RenderResourceFlowTable();
    void RenderWarningsPanel();

    /// @brief Draw the imported factory as a read-only node graph (own editor
    /// context). Applies imported node positions + fits the view on first frame
    /// after a load. No edit/create/delete handlers are wired.
    void RenderGraphCanvas();
    /// @brief Draw one node: title + per-pin item & imported rate. Read-only.
    void RenderSnapshotNode(const Node& node);
    /// @brief Draw the links between node pins (purely visual).
    void RenderSnapshotLinks();
```

(Keep whatever order the existing render-method declarations use; the key is the three new declarations exist in `private:`.)

Add the members after `unsigned long long int next_id = 1;`:

```cpp
    unsigned long long int next_id = 1;

    ax::NodeEditor::Config config;
    ax::NodeEditor::EditorContext* context = nullptr;
    /// @brief Set after a successful import; consumed on the next canvas frame to
    /// apply imported node positions and fit the view.
    bool needs_layout_apply = false;
```

- [ ] **Step 2: Create the editor context in the constructor, destroy it in the destructor**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, replace the constructor and the defaulted destructor:

```cpp
FactorySnapshotApp::FactorySnapshotApp()
{
    LoadSession();
}

FactorySnapshotApp::~FactorySnapshotApp() = default;
```

with:

```cpp
FactorySnapshotApp::FactorySnapshotApp()
{
    LoadSession();
    config.SettingsFile = nullptr;     // no on-disk layout file
    config.EnableSmoothZoom = true;
    context = ax::NodeEditor::CreateEditor(&config);
}

FactorySnapshotApp::~FactorySnapshotApp()
{
    ax::NodeEditor::DestroyEditor(context);
}
```

- [ ] **Step 3: Add the rendering includes and helpers**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, add to the includes (after `#include "domain/node.hpp"`):

```cpp
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/node_display.hpp"
```

Add these file-local helpers inside the existing anonymous `namespace { ... }` block (next to `KindLabel`):

```cpp
    // A small connection marker; the node editor attaches links to the pin rect.
    void DrawPinMarker()
    {
        const float r = 4.0f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + r), r, IM_COL32(200, 200, 200, 255));
    }

    // "<item>  <rate>/min" using the imported rate (no balancing).
    std::string PinLabel(const Pin& pin)
    {
        const std::string item = pin.item ? pin.item->name : std::string("(none)");
        return item + "  " + pin.current_rate.GetStringFraction() + "/min";
    }
```

- [ ] **Step 4: Implement the three render methods**

Add these definitions to `ficsit-companion/src/app/factory_snapshot_app.cpp` (e.g. after `RenderResourceFlowTable`):

```cpp
void FactorySnapshotApp::RenderGraphCanvas()
{
    ax::NodeEditor::SetCurrentEditor(context);

    if (needs_layout_apply)
    {
        for (const auto& node : model.nodes)
        {
            ax::NodeEditor::SetNodePosition(node->id, node->pos);
        }
    }

    ax::NodeEditor::Begin("##snapshot_graph", ImGui::GetContentRegionAvail());

    for (const auto& node : model.nodes)
    {
        RenderSnapshotNode(*node);
    }
    RenderSnapshotLinks();

    ax::NodeEditor::End();

    if (needs_layout_apply)
    {
        ax::NodeEditor::NavigateToContent();
        needs_layout_apply = false;
    }

    ax::NodeEditor::SetCurrentEditor(nullptr);
}

void FactorySnapshotApp::RenderSnapshotNode(const Node& node)
{
    ax::NodeEditor::BeginNode(node.id);

    ImGui::TextUnformatted(NodeDisplayName(node).c_str());

    ImGui::BeginGroup(); // inputs: marker then label
    for (const auto& pin : node.ins)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        DrawPinMarker();
        ImGui::SameLine();
        ImGui::TextUnformatted(PinLabel(*pin).c_str());
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup(); // outputs: label then marker
    for (const auto& pin : node.outs)
    {
        ax::NodeEditor::BeginPin(pin->id, pin->direction);
        ImGui::TextUnformatted(PinLabel(*pin).c_str());
        ImGui::SameLine();
        DrawPinMarker();
        ax::NodeEditor::EndPin();
    }
    ImGui::EndGroup();

    ax::NodeEditor::EndNode();
}

void FactorySnapshotApp::RenderSnapshotLinks()
{
    for (const auto& link : model.links)
    {
        if (!link) continue;
        ax::NodeEditor::Link(link->id, link->start_id, link->end_id);
    }
}
```

- [ ] **Step 5: Build the GUI target to confirm it compiles**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: links to `...\Release\ficsit-companion.exe` with no errors. (Methods are defined but not yet called — no visible change.)

- [ ] **Step 6: Run the full test suite (no regressions)**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: 131 tests pass, 0 failed.

- [ ] **Step 7: No commit — tick this task in the PROGRESS notes.**

---

### Task 3: Wire the graph into the layout and trigger layout on load

Restructure `RenderImpl` so the graph fills the main area with a left info panel, move the resource-flow table into a collapsible, and flag a layout apply on each successful import.

**Files:**
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp`

**Interfaces:**
- Consumes: `RenderGraphCanvas()` (Task 2), `needs_layout_apply` (Task 2), existing `RenderStatusLine()/RenderTopologySummary()/RenderWarningsPanel()/RenderResourceFlowTable()`.

- [ ] **Step 1: Flag a layout apply at the end of a successful import**

In `FactorySnapshotApp::LoadFromWrapperJson`, after `status_text = st.str();` (the success path), add:

```cpp
    status_text = st.str();
    needs_layout_apply = true; // apply imported node positions + fit on next canvas frame
}
```

- [ ] **Step 2: Add a bounded, horizontally-scrollable flow table**

In `RenderResourceFlowTable`, change the `BeginTable` call from:

```cpp
    if (ImGui::BeginTable("##flow_table", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
```

to (adds horizontal scroll and a bounded height so it fits inside the narrow left panel):

```cpp
    if (ImGui::BeginTable("##flow_table", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
        ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f)))
```

- [ ] **Step 3: Rewrite `RenderImpl` to the graph-main + left-panel layout**

Replace the whole `FactorySnapshotApp::RenderImpl` body:

```cpp
void FactorySnapshotApp::RenderImpl()
{
    RenderStatusLine();
    ImGui::Separator();

    ImGui::BeginChild("##snapshot_left", ImVec2(280.0f, 0.0f), true);
    RenderTopologySummary();
    ImGui::Separator();
    RenderWarningsPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##snapshot_right", ImVec2(0.0f, 0.0f), true);
    RenderResourceFlowTable();
    ImGui::EndChild();
}
```

with:

```cpp
void FactorySnapshotApp::RenderImpl()
{
    RenderStatusLine();
    ImGui::Separator();

    ImGui::BeginChild("##snapshot_left", ImVec2(340.0f, 0.0f), true);
    RenderTopologySummary();
    ImGui::Separator();
    RenderWarningsPanel();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Resource flow"))
    {
        RenderResourceFlowTable();
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
```

- [ ] **Step 4: Build the GUI target**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: links to `...\Release\ficsit-companion.exe` with no errors.

- [ ] **Step 5: Run the full test suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: 131 tests pass, 0 failed.

- [ ] **Step 6: Manual GUI smoke test**

Launch `ficsit-companion.exe`, set the Save folder, pick a save from the dropdown, open the **Factory Snapshot** tab. Confirm:
- Nodes draw with titles (recipe / extractor / Merger / Splitter / Sink / station names).
- Each pin shows `item  rate/min` (rates as imported).
- Links connect pins; the view fits to content on first load.
- Pan / zoom / drag-node work; nothing persists (reopening re-fits from import).
- The left panel's **Resource flow** collapsible still filters/searches/scrolls.

- [ ] **Step 7: No commit — tick this task and mark the feature done in the PROGRESS notes.**

---

## Self-Review

**Spec coverage:**
- Own editor context (create/destroy, `SettingsFile=nullptr`) — Task 2 Step 2. ✓
- Read-only (no create/delete handlers, balancer never called) — Tasks 2–3 wire only draw calls. ✓
- `RenderGraphCanvas` / `RenderSnapshotNode` / `RenderSnapshotLinks` — Task 2 Step 4. ✓
- Node title helper by kind — Task 1 (`NodeDisplayName`). ✓
- Per-pin item + imported rate, input marker-then-label / output label-then-marker — Task 2 Steps 3–4. ✓
- Pin marker as a simple circle (no item-icon texture) — Task 2 Step 3 (`DrawPinMarker`). ✓
- Layout apply + fit via `needs_layout_apply` set in `LoadFromWrapperJson`, consumed in canvas — Task 2 Step 4, Task 3 Step 1. ✓
- Layout: status + 340px left panel (topology, warnings, collapsible resource flow w/ ScrollX) + graph main + empty state — Task 3 Steps 2–3. ✓
- Unit test for the pure helper (organizer/sink kinds, no Data/editor) — Task 1. ✓
- CMake wiring for new domain source/header/test — Task 1 Step 1. ✓

**Placeholder scan:** none — every code/command step has concrete content.

**Type consistency:** `NodeDisplayName(const Node&)→std::string` used identically in Task 1 and Task 2; `RenderGraphCanvas/RenderSnapshotNode/RenderSnapshotLinks` declared (Task 2 Step 1) and defined (Task 2 Step 4) with matching signatures; `needs_layout_apply` set (Task 3) matches member (Task 2). `Link::start_id/end_id` and `Pin::direction/current_rate/item` match the verified struct definitions.
