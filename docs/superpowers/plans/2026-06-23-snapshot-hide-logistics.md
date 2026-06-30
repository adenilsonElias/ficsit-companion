# Snapshot: Hide & Bypass Splitters / Mergers / Logistics — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a snapshot-view option that hides splitter/merger/logistics nodes and re-routes flow so producers connect directly to consumers, for cleaner visualization — without mutating the read-only imported model.

**Architecture:** A pure domain function (`ComputeVisibleEdges`) walks the node/link graph and, given an "is this node hidden?" predicate, returns the set of effective edges between visible-node pins (transitively bypassing hidden nodes). The snapshot app stores per-kind toggle flags in its session, skips hidden nodes when rendering, and draws a cached effective-edge list instead of the raw links.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, Catch2 tests, CMake.

## Global Constraints

- The imported model (`FactorySnapshotModel`) is **never mutated**; the feature is render-only and toggleable.
- Topology summary and Resource-flow table must keep showing **full** imported data — the toggle affects only the graph canvas.
- New session keys are **additive**; older sessions missing them fall back to defaults (per-key merge on deserialize).
- Node kinds in scope: `Node::Kind::GameSplitter`, `CustomSplitter`, `Merger`, `Logistics`.
- **Project preference: NO git commits.** Do not run `git commit`/`branch`/etc. Instead, after each task check off its steps here and append a one-line entry to `docs/superpowers/plans/2026-06-23-snapshot-hide-logistics-PROGRESS.md`.
- Layer rule: `domain/` has zero UI/IO dependencies; `infra/` may depend on `domain/`; `app/` may depend on both.

## File Structure

- Create `ficsit-companion/include/domain/snapshot/snapshot_bypass.hpp` — `SnapshotEdge` struct + `ComputeVisibleEdges` declaration.
- Create `ficsit-companion/src/domain/snapshot/snapshot_bypass.cpp` — traversal implementation.
- Create `ficsit-companion/tests/test_snapshot_bypass.cpp` — Catch2 coverage of the traversal.
- Modify `ficsit-companion/CMakeLists.txt` — register the new header, source, and test.
- Modify `ficsit-companion/include/infra/persistence/factory_snapshot_session.hpp` — 5 new bool fields.
- Modify `ficsit-companion/src/infra/persistence/factory_snapshot_session.cpp` — serialize/deserialize them.
- Modify `ficsit-companion/tests/test_factory_snapshot_session.cpp` — round-trip + default coverage.
- Modify `ficsit-companion/include/app/factory_snapshot_app.hpp` — edge cache members + helper decls.
- Modify `ficsit-companion/src/app/factory_snapshot_app.cpp` — predicate, Options UI, canvas integration.

---

### Task 1: Domain bypass traversal

**Files:**
- Create: `ficsit-companion/include/domain/snapshot/snapshot_bypass.hpp`
- Create: `ficsit-companion/src/domain/snapshot/snapshot_bypass.cpp`
- Test: `ficsit-companion/tests/test_snapshot_bypass.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct SnapshotEdge { ax::NodeEditor::PinId start_id; ax::NodeEditor::PinId end_id; };`
  - `std::vector<SnapshotEdge> ComputeVisibleEdges(const std::vector<std::unique_ptr<Node>>& nodes, const std::vector<std::unique_ptr<Link>>& links, const std::function<bool(const Node&)>& is_hidden);`

- [ ] **Step 1: Register new files in CMake**

In `ficsit-companion/CMakeLists.txt`, add the header to `HEADER_FILES` (after line 19, `include/domain/snapshot/resource_flow.hpp`):

```cmake
	include/domain/snapshot/snapshot_bypass.hpp
```

Add the source to `DOMAIN_SOURCE_FILES` (after line 75, `src/domain/snapshot/resource_flow.cpp`):

```cmake
    src/domain/snapshot/snapshot_bypass.cpp
```

Add the test to `TEST_SOURCE_FILES` (after line 223, `tests/test_resource_flow.cpp`):

```cmake
        tests/test_snapshot_bypass.cpp
```

- [ ] **Step 2: Write the header**

Create `ficsit-companion/include/domain/snapshot/snapshot_bypass.hpp`:

```cpp
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <imgui_node_editor.h>

struct Node;
struct Link;

/// @brief An effective canvas edge after hidden nodes are bypassed. Both pins
/// belong to *visible* nodes; the edge may correspond to a real link or to a
/// path routed transitively through one or more hidden nodes.
struct SnapshotEdge
{
    ax::NodeEditor::PinId start_id; // output pin on a visible node
    ax::NodeEditor::PinId end_id;   // input pin on a visible node
};

/// @brief Compute the edges to draw when some nodes are hidden. For every link
/// whose producer (start) node is visible: if the consumer (end) node is also
/// visible, the link is emitted unchanged; if the consumer is hidden, the graph
/// is walked forward through hidden nodes (DFS, cycle-guarded) until visible
/// input pins are reached, emitting one edge per reached pin. Links starting at
/// a hidden node are skipped (they are reached via the forward walk instead).
/// Hidden nodes with no visible downstream produce no edge. Duplicate edges
/// (same start+end) are collapsed. The model is not modified.
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden);
```

- [ ] **Step 3: Write the failing test**

Create `ficsit-companion/tests/test_snapshot_bypass.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"
#include "domain/snapshot/snapshot_bypass.hpp"

#include "graph_test_helpers.hpp" // IdGen, MakeLink

namespace
{
    // Builds organizer nodes (splitter = 1-in/3-out, merger = 3-in/1-out) and
    // wires them with MakeLink. "Visibility" is chosen per-node by pointer so a
    // test controls topology independent of node kind (the production predicate
    // is kind-based, but ComputeVisibleEdges takes an arbitrary predicate).
    struct BypassFixture
    {
        IdGen idgen;
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<std::unique_ptr<Link>> links;
        std::unordered_set<const Node*> hidden;

        CustomSplitterNode* AddSplitter()
        {
            auto n = std::make_unique<CustomSplitterNode>(
                ax::NodeEditor::NodeId(idgen()), [this] { return idgen(); }, nullptr);
            CustomSplitterNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        MergerNode* AddMerger()
        {
            auto n = std::make_unique<MergerNode>(
                ax::NodeEditor::NodeId(idgen()), [this] { return idgen(); }, nullptr);
            MergerNode* raw = n.get();
            nodes.push_back(std::move(n));
            return raw;
        }
        void Connect(Pin* out_pin, Pin* in_pin)
        {
            links.push_back(MakeLink(idgen(), out_pin, in_pin));
        }
        void Hide(const Node* n) { hidden.insert(n); }

        std::vector<SnapshotEdge> Run()
        {
            return ComputeVisibleEdges(nodes, links,
                [this](const Node& n) { return hidden.count(&n) != 0; });
        }
        static bool Has(const std::vector<SnapshotEdge>& es,
                        const Pin* start, const Pin* end)
        {
            for (const auto& e : es)
                if (e.start_id.Get() == start->id.Get() &&
                    e.end_id.Get()   == end->id.Get()) return true;
            return false;
        }
    };
}

/// @test A visible->visible link passes through unchanged.
TEST_CASE("visible link is emitted unchanged", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A hidden splitter is bypassed; the producer connects to each consumer.
TEST_CASE("hidden splitter fans the producer to all consumers", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    CustomSplitterNode* h  = fx.AddSplitter(); fx.Hide(h);
    MergerNode* c1 = fx.AddMerger();
    MergerNode* c2 = fx.AddMerger();
    fx.Connect(p->outs[0].get(), h->ins[0].get());
    fx.Connect(h->outs[0].get(), c1->ins[0].get());
    fx.Connect(h->outs[1].get(), c2->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 2);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c1->ins[0].get()));
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c2->ins[0].get()));
}

/// @test A hidden merger is bypassed; each producer connects to the consumer.
TEST_CASE("hidden merger joins all producers to the consumer", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p1 = fx.AddSplitter();
    CustomSplitterNode* p2 = fx.AddSplitter();
    MergerNode* h = fx.AddMerger(); fx.Hide(h);
    MergerNode* c = fx.AddMerger();
    fx.Connect(p1->outs[0].get(), h->ins[0].get());
    fx.Connect(p2->outs[0].get(), h->ins[1].get());
    fx.Connect(h->outs[0].get(),  c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 2);
    REQUIRE(BypassFixture::Has(edges, p1->outs[0].get(), c->ins[0].get()));
    REQUIRE(BypassFixture::Has(edges, p2->outs[0].get(), c->ins[0].get()));
}

/// @test A chain of consecutive hidden nodes collapses to one direct edge.
TEST_CASE("hidden chain collapses to a direct edge", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    CustomSplitterNode* h1 = fx.AddSplitter(); fx.Hide(h1);
    MergerNode* h2 = fx.AddMerger(); fx.Hide(h2);
    MergerNode* c  = fx.AddMerger();
    fx.Connect(p->outs[0].get(),  h1->ins[0].get());
    fx.Connect(h1->outs[0].get(), h2->ins[0].get());
    fx.Connect(h2->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A hidden node with no visible downstream produces no edge.
TEST_CASE("hidden endpoint disappears", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* h = fx.AddMerger(); fx.Hide(h);
    fx.Connect(p->outs[0].get(), h->ins[0].get());

    auto edges = fx.Run();
    REQUIRE(edges.empty());
}

/// @test A cycle among hidden nodes terminates (visited guard) and yields none.
TEST_CASE("cyclic hidden nodes terminate", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p  = fx.AddSplitter();
    MergerNode* h1 = fx.AddMerger();        fx.Hide(h1);
    CustomSplitterNode* h2 = fx.AddSplitter(); fx.Hide(h2);
    fx.Connect(p->outs[0].get(),  h1->ins[0].get());
    fx.Connect(h1->outs[0].get(), h2->ins[0].get());
    fx.Connect(h2->outs[0].get(), h1->ins[1].get()); // back-edge

    auto edges = fx.Run();
    REQUIRE(edges.empty());
}

/// @test With nothing hidden, every link is emitted unchanged.
TEST_CASE("no hidden nodes leaves links untouched", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), c->ins[0].get());

    auto edges = fx.Run(); // hidden set empty
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}
```

- [ ] **Step 4: Run the test to verify it fails to build/link**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `snapshot_bypass.cpp` missing / `ComputeVisibleEdges` unresolved.

- [ ] **Step 5: Write the implementation**

Create `ficsit-companion/src/domain/snapshot/snapshot_bypass.cpp`:

```cpp
#include "domain/snapshot/snapshot_bypass.hpp"

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"

#include <set>
#include <unordered_set>
#include <utility>

namespace
{
    // Walk forward from a hidden node, collecting the ids of every visible input
    // pin reachable through chains of hidden nodes. `visited` guards cycles.
    void CollectVisibleSinks(const Node& hidden_node,
                             const std::function<bool(const Node&)>& is_hidden,
                             std::unordered_set<const Node*>& visited,
                             std::vector<ax::NodeEditor::PinId>& sinks)
    {
        if (!visited.insert(&hidden_node).second) return;
        for (const auto& out : hidden_node.outs)
        {
            if (!out || !out->link) continue;
            const Pin* end = out->link->end;
            if (!end || !end->node) continue;
            if (is_hidden(*end->node))
                CollectVisibleSinks(*end->node, is_hidden, visited, sinks);
            else
                sinks.push_back(end->id);
        }
    }
}

std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& /*nodes*/,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden)
{
    std::vector<SnapshotEdge> edges;
    std::set<std::pair<uintptr_t, uintptr_t>> seen; // dedupe (start,end) pin ids

    auto emit = [&](ax::NodeEditor::PinId start, ax::NodeEditor::PinId end)
    {
        if (seen.insert({ start.Get(), end.Get() }).second)
            edges.push_back({ start, end });
    };

    for (const auto& link : links)
    {
        if (!link || !link->start || !link->end) continue;
        const Pin* start = link->start;
        const Pin* end = link->end;
        if (!start->node || !end->node) continue;
        if (is_hidden(*start->node)) continue; // reached via forward walk instead

        if (!is_hidden(*end->node))
        {
            emit(start->id, end->id);
        }
        else
        {
            std::unordered_set<const Node*> visited;
            std::vector<ax::NodeEditor::PinId> sinks;
            CollectVisibleSinks(*end->node, is_hidden, visited, sinks);
            for (const auto& sink : sinks) emit(start->id, sink);
        }
    }
    return edges;
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[snapshot_bypass]"`
Expected: PASS — all `[snapshot_bypass]` test cases green.

- [ ] **Step 7: Record progress**

Append to `docs/superpowers/plans/2026-06-23-snapshot-hide-logistics-PROGRESS.md`:
`Task 1 done — ComputeVisibleEdges + tests passing.`

---

### Task 2: Session persistence

**Files:**
- Modify: `ficsit-companion/include/infra/persistence/factory_snapshot_session.hpp`
- Modify: `ficsit-companion/src/infra/persistence/factory_snapshot_session.cpp`
- Test: `ficsit-companion/tests/test_factory_snapshot_session.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `FactorySnapshotSession` fields `hide_logistics_enabled`, `hide_game_splitters`, `hide_custom_splitters`, `hide_mergers`, `hide_logistics_nodes` (all `bool`).

- [ ] **Step 1: Write the failing test**

In `ficsit-companion/tests/test_factory_snapshot_session.cpp`, extend the existing round-trip test body (after `in.icon_scale = 2.5f;` at line 14, before constructing `out`) by adding:

```cpp
    in.hide_logistics_enabled = true;
    in.hide_game_splitters = false;
    in.hide_custom_splitters = true;
    in.hide_mergers = false;
    in.hide_logistics_nodes = true;
```

And add these assertions after the existing `REQUIRE(out.icon_scale == in.icon_scale);` (line 23):

```cpp
    REQUIRE(out.hide_logistics_enabled == in.hide_logistics_enabled);
    REQUIRE(out.hide_game_splitters == in.hide_game_splitters);
    REQUIRE(out.hide_custom_splitters == in.hide_custom_splitters);
    REQUIRE(out.hide_mergers == in.hide_mergers);
    REQUIRE(out.hide_logistics_nodes == in.hide_logistics_nodes);
```

Then add a new test case at the end of the file proving defaults survive a JSON that omits the keys:

```cpp
/// @test A session JSON without the hide-* keys leaves their defaults intact.
TEST_CASE("FactorySnapshotSession keeps hide defaults when keys absent", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"flow_filter\": 1}");
    REQUIRE(s.hide_logistics_enabled == false);
    REQUIRE(s.hide_game_splitters == true);
    REQUIRE(s.hide_custom_splitters == true);
    REQUIRE(s.hide_mergers == true);
    REQUIRE(s.hide_logistics_nodes == true);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `hide_logistics_enabled` etc. are not members of `FactorySnapshotSession`.

- [ ] **Step 3: Add the fields to the header**

In `ficsit-companion/include/infra/persistence/factory_snapshot_session.hpp`, after the `collapsed_font_scale` field (line 22) add:

```cpp

    /// @brief Snapshot graph-view "hide & bypass logistics" preferences. Master
    /// toggle off by default; per-kind selections default on so enabling the
    /// master immediately hides all four kinds. Visualization-only (model and
    /// resource-flow report are unaffected).
    bool hide_logistics_enabled = false;
    bool hide_game_splitters = true;
    bool hide_custom_splitters = true;
    bool hide_mergers = true;
    bool hide_logistics_nodes = true;
```

- [ ] **Step 4: Serialize and deserialize the fields**

In `ficsit-companion/src/infra/persistence/factory_snapshot_session.cpp`, in `Serialize()` after the `collapsed_font_scale` line (line 17) add:

```cpp
    v["hide_logistics_enabled"] = hide_logistics_enabled;
    v["hide_game_splitters"] = hide_game_splitters;
    v["hide_custom_splitters"] = hide_custom_splitters;
    v["hide_mergers"] = hide_mergers;
    v["hide_logistics_nodes"] = hide_logistics_nodes;
```

In `Deserialize()` after the `collapsed_font_scale` line (line 33) add:

```cpp
    if (v.contains("hide_logistics_enabled") && v["hide_logistics_enabled"].is_bool()) hide_logistics_enabled = v["hide_logistics_enabled"].get<bool>();
    if (v.contains("hide_game_splitters") && v["hide_game_splitters"].is_bool()) hide_game_splitters = v["hide_game_splitters"].get<bool>();
    if (v.contains("hide_custom_splitters") && v["hide_custom_splitters"].is_bool()) hide_custom_splitters = v["hide_custom_splitters"].get<bool>();
    if (v.contains("hide_mergers") && v["hide_mergers"].is_bool()) hide_mergers = v["hide_mergers"].get<bool>();
    if (v.contains("hide_logistics_nodes") && v["hide_logistics_nodes"].is_bool()) hide_logistics_nodes = v["hide_logistics_nodes"].get<bool>();
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[snapshot_session]"`
Expected: PASS — all `[snapshot_session]` cases green.

- [ ] **Step 6: Record progress**

Append to the PROGRESS.md file:
`Task 2 done — session hide-* fields persisted + tests passing.`

---

### Task 3: App integration (predicate, Options UI, canvas)

**Files:**
- Modify: `ficsit-companion/include/app/factory_snapshot_app.hpp`
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp`

**Interfaces:**
- Consumes: `ComputeVisibleEdges` / `SnapshotEdge` (Task 1); session `hide_*` fields (Task 2).
- Produces: no new public interface (UI + private members only).

This task is UI/render wiring; it is verified by build + manual run, not a unit test (consistent with the existing snapshot app, which has no app-level unit tests).

- [ ] **Step 1: Add cache members and helper declarations to the header**

In `ficsit-companion/include/app/factory_snapshot_app.hpp`, add the include near the others (after line 8, `#include "domain/snapshot/factory_snapshot_model.hpp"`):

```cpp
#include "domain/snapshot/snapshot_bypass.hpp"
```

Add a private helper declaration after `void RenderSnapshotLinks();` (line 57):

```cpp
    /// @brief True when `node` should be hidden under the current session
    /// hide-* toggles (master off => always false).
    bool IsNodeHidden(const Node& node) const;
    /// @brief Recompute `visible_edges` from the model under the current hide
    /// toggles, assigning each edge a stable editor link id from NextId().
    void RebuildVisibleEdges();
```

Add private members after `bool needs_layout_apply = false;` (line 70):

```cpp

    /// @brief Cached effective edges to draw (hidden nodes bypassed). Each entry
    /// pairs a SnapshotEdge with a stable editor link id. Rebuilt on import and
    /// whenever a hide toggle changes.
    struct CachedEdge { SnapshotEdge edge; ax::NodeEditor::LinkId id; };
    std::vector<CachedEdge> visible_edges;
```

- [ ] **Step 2: Implement the predicate and cache rebuild**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, add these two methods immediately above `void FactorySnapshotApp::RenderSnapshotLinks()` (line 624):

```cpp
bool FactorySnapshotApp::IsNodeHidden(const Node& node) const
{
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
    visible_edges.clear();
    const std::vector<SnapshotEdge> edges = ComputeVisibleEdges(
        model.nodes, model.links,
        [this](const Node& n) { return IsNodeHidden(n); });
    visible_edges.reserve(edges.size());
    for (const SnapshotEdge& e : edges)
        visible_edges.push_back({ e, ax::NodeEditor::LinkId(NextId()) });
}
```

- [ ] **Step 3: Draw the cached edges instead of raw links, and skip hidden nodes**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, replace the body of `RenderSnapshotLinks()` (lines 624-631) with:

```cpp
void FactorySnapshotApp::RenderSnapshotLinks()
{
    for (const CachedEdge& ce : visible_edges)
    {
        ax::NodeEditor::Link(ce.id, ce.edge.start_id, ce.edge.end_id);
    }
}
```

In `RenderGraphCanvas`, change the node render loop (lines 412-415) to skip hidden nodes:

```cpp
    for (const auto& node : model.nodes)
    {
        if (IsNodeHidden(*node)) continue;
        RenderSnapshotNode(*node);
    }
```

- [ ] **Step 4: Rebuild the cache on import**

In `RenderGraphCanvas`, inside the `if (needs_layout_apply)` block at the top (lines 395-401), after the position-apply loop add a rebuild so a fresh import populates the cache:

```cpp
    if (needs_layout_apply)
    {
        for (const auto& node : model.nodes)
        {
            ax::NodeEditor::SetNodePosition(node->id, node->pos);
        }
        RebuildVisibleEdges();
    }
```

- [ ] **Step 5: Add the Options-panel controls**

In `RenderOptionsPanel`, after the `if (changed) SaveSession();` line (line 328) and before the `ImGui::Spacing();`/`Reset to defaults` block (line 330), insert:

```cpp
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
```

- [ ] **Step 6: Reset hide-toggles in "Reset to defaults"**

In `RenderOptionsPanel`, inside the `if (ImGui::Button("Reset to defaults"))` block (lines 331-337), after `session.collapsed_font_scale = 1.0f;` and before `SaveSession();`, add:

```cpp
        session.hide_logistics_enabled = false;
        session.hide_game_splitters = true;
        session.hide_custom_splitters = true;
        session.hide_mergers = true;
        session.hide_logistics_nodes = true;
        RebuildVisibleEdges();
```

- [ ] **Step 7: Build the full app and tests**

Run: `cmake --build build --config Release`
Expected: PASS — clean build of the app and `fc-tests`.

Then: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS — all tests green.

- [ ] **Step 8: Manual verification**

Launch the app, import a `.sav` with splitters/mergers, open the Factory Snapshot tool's Options tab. Confirm:
- Master toggle off → graph identical to before (all nodes + links).
- Master toggle on → splitters/mergers/logistics disappear and remaining machines stay connected by direct links (no dangling consumers, except true endpoints which simply vanish).
- Per-kind checkboxes disable/enable when master is off/on, and toggling each kind updates the canvas.
- Topology summary counts and Resource-flow table are unchanged by the toggles.
- Re-open the tool (or restart) → toggle state persists.

- [ ] **Step 9: Record progress**

Append to the PROGRESS.md file:
`Task 3 done — Options UI + canvas bypass wired; full build + ctest green; manual check passed.`

---

## Self-Review

**Spec coverage:**
- Bypass / transitive re-route → Task 1 (`ComputeVisibleEdges` + chain/fan tests). ✓
- Endpoint-safe (hidden node, no visible downstream) → Task 1 "hidden endpoint disappears". ✓
- Four node kinds in scope → Task 3 `IsNodeHidden` switch. ✓
- Master toggle + per-kind UI → Task 3 Step 5. ✓
- Persistence (additive keys, default-preserving) → Task 2. ✓
- Visualization-only (topology/flow untouched) → Task 3 touches only node loop + links; verified in Step 8. ✓
- Model never mutated → `ComputeVisibleEdges` takes const refs; app skips render only. ✓
- Stable per-frame link ids → Task 3 cache rebuilt only on import / toggle change. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows full code. ✓

**Type consistency:** `ComputeVisibleEdges` / `SnapshotEdge` signatures match between Task 1 header, Task 1 test, and Task 3 usage. Session field names identical across Task 2 header/cpp/test and Task 3 predicate/UI. `CachedEdge`/`visible_edges` defined in Task 3 Step 1 and used in Steps 2-3. ✓
