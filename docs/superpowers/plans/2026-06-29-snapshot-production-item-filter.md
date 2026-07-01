# Snapshot Per-Item Production Filter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-item visibility checkboxes to the Factory Snapshot's Resources tab so unchecking a produced item hides (pure-hide, no reroute) the machines producing it on the read-only graph canvas, with Select all / Unselect all.

**Architecture:** Render-time filtering only — the imported model stays read-only. A persisted set of *hidden* item names drives a per-node predicate (`NodeProductionHidden`); the snapshot edge builder is extended so production-hidden nodes are *dropped* (their links vanish) while existing logistics-hidden nodes still *bypass* (reroute). The Resources flow table gains a checkbox column plus Select/Unselect-all buttons.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, Catch2 tests, custom `Json::` library, CMake.

## Global Constraints

- The imported `FactorySnapshotModel` is read-only; never mutate `model.nodes`/`model.links`. All filtering is visualization-only.
- Persistence keys are additive — a session JSON missing the new key must keep the default (empty set = everything visible).
- Producer definition is fixed: a node "produces" item X iff `node.IsCraft() || node.IsExtractor()` and it has an output pin whose `item->name == X` (same rule as `NodesProducingItem`).
- A producer is hidden only when *every* one of its output items is unchecked; if any output item is still visible the node stays.
- Default state = all visible (the persisted hidden set is empty by default).
- **No git commits** — this repo's owner does not want git operations performed automatically. Do NOT run `git add`/`git commit`. Track task completion in `docs/superpowers/plans/2026-06-29-snapshot-production-item-filter-PROGRESS.md` instead (the executing skill manages this file).

### Build & test reference (Windows / PowerShell)

Configure once (if `build/` does not yet exist):
```
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
```
Build the test target:
```
cmake --build build --config Release --target fc-tests
```
Run all tests:
```
ctest --test-dir build -C Release --output-on-failure
```
Run one Catch2 tag directly (faster while iterating), e.g.:
```
build\ficsit-companion\Release\fc-tests.exe "[snapshot_session]"
```

---

### Task 1: Persist the hidden-production-items set

**Files:**
- Modify: `ficsit-companion/include/infra/persistence/factory_snapshot_session.hpp`
- Modify: `ficsit-companion/src/infra/persistence/factory_snapshot_session.cpp`
- Test: `ficsit-companion/tests/test_factory_snapshot_session.cpp`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `FactorySnapshotSession::hidden_production_items` — a `std::vector<std::string>` of item names that are hidden; round-trips through `Serialize()`/`Deserialize()` under JSON key `"hidden_production_items"`.

- [ ] **Step 1: Write the failing tests**

Add to `ficsit-companion/tests/test_factory_snapshot_session.cpp`:

```cpp
/// @test hidden_production_items survives Serialize -> Deserialize.
TEST_CASE("FactorySnapshotSession round-trips hidden_production_items", "[snapshot_session]")
{
    FactorySnapshotSession in;
    in.hidden_production_items = { "Iron Ingot", "Screw" };

    FactorySnapshotSession out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.hidden_production_items == in.hidden_production_items);
}

/// @test A session JSON without the key leaves hidden_production_items empty.
TEST_CASE("FactorySnapshotSession defaults hidden_production_items to empty", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"flow_filter\": 1}");
    REQUIRE(s.hidden_production_items.empty());
}

/// @test Non-string array entries are skipped on deserialize.
TEST_CASE("FactorySnapshotSession ignores non-string hidden items", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"hidden_production_items\": [\"Iron Ingot\", 7, true]}");
    REQUIRE(s.hidden_production_items == std::vector<std::string>{ "Iron Ingot" });
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Release --target fc-tests`
Expected: compile error — `hidden_production_items` is not a member of `FactorySnapshotSession`.

- [ ] **Step 3: Add the field to the header**

In `factory_snapshot_session.hpp`, add `#include <vector>` next to the existing `#include <string>`, then add below the `hide_logistics_nodes` field (line 32):

```cpp
    /// @brief Item names whose producers (Craft/Extractor) are hidden on the
    /// snapshot canvas. Empty by default => every production visible. Storing the
    /// *hidden* set (not the visible one) keeps the default empty and makes newly
    /// imported items visible automatically. Visualization-only.
    std::vector<std::string> hidden_production_items;
```

- [ ] **Step 4: Serialize and deserialize the field**

In `factory_snapshot_session.cpp`, in `Serialize()` after the `hide_logistics_nodes` line (line 22), add:

```cpp
    Json::Array hidden_items;
    for (const std::string& name : hidden_production_items)
        hidden_items.push_back(Json::Value(name));
    v["hidden_production_items"] = hidden_items;
```

In `Deserialize()`, after the `hide_logistics_nodes` read line (line 43), add:

```cpp
    if (v.contains("hidden_production_items") && v["hidden_production_items"].is_array())
    {
        hidden_production_items.clear();
        for (const auto& e : v["hidden_production_items"].get_array())
            if (e.is_string()) hidden_production_items.push_back(e.get_string());
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests` then
`build\ficsit-companion\Release\fc-tests.exe "[snapshot_session]"`
Expected: PASS (all `[snapshot_session]` cases, including the three new ones).

- [ ] **Step 6: Mark Task 1 complete in the PROGRESS file.**

---

### Task 2: `NodeProductionHidden` domain predicate

**Files:**
- Modify: `ficsit-companion/include/domain/nodes/node_display.hpp`
- Modify: `ficsit-companion/src/domain/nodes/node_display.cpp`
- Test: `ficsit-companion/tests/test_node_display.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks (uses existing `Node`, `Pin`, `Item`).
- Produces: `bool NodeProductionHidden(const Node& node, const std::set<std::string>& hidden_items);` — true iff `node` is a Craft/Extractor producer with at least one output item and *every* output item name is in `hidden_items`.

- [ ] **Step 1: Write the failing tests**

Add to `ficsit-companion/tests/test_node_display.cpp` (add `#include <set>` near the other includes at the top):

```cpp
/// @test NodeProductionHidden hides a single-output producer when its item is
/// in the hidden set, keeps it otherwise, and never hides non-producers.
/// @covers NodeProductionHidden producer/non-producer + hidden-set membership.
TEST_CASE("NodeProductionHidden hides producers whose every output is hidden", "[node_display]")
{
    const Item iron_ore("Iron Ore", "", 0);

    // Single-output producer (a miner).
    ExtractorNode miner(60, ExtractorNode::Kind::MinerMk1, &iron_ore, ExtractorNode::Purity::Normal, Gen);

    REQUIRE(NodeProductionHidden(miner, { "Iron Ore" }) == true);
    REQUIRE(NodeProductionHidden(miner, { "Copper Ore" }) == false);
    REQUIRE(NodeProductionHidden(miner, {}) == false);

    // A non-producer (merger) is never hidden by this predicate, even if it
    // carries the item.
    MergerNode merger(61, Gen, &iron_ore);
    REQUIRE(NodeProductionHidden(merger, { "Iron Ore" }) == false);
}

/// @test A multi-output producer stays visible while any one output item is
/// still visible, and hides only once all of its outputs are hidden.
/// @covers NodeProductionHidden "all outputs hidden" rule.
TEST_CASE("NodeProductionHidden keeps multi-output producers with a visible output", "[node_display]")
{
    const Item iron_ingot("Iron Ingot", "", 0);
    const Item slag("Slag", "", 0);

    // Build a producer with two distinct output items: a miner (1 output) plus a
    // second manually-added output pin. ExtractorNode IsExtractor() => producer.
    ExtractorNode producer(70, ExtractorNode::Kind::MinerMk1, &iron_ingot, ExtractorNode::Purity::Normal, Gen);
    producer.outs.push_back(std::make_unique<Pin>(
        ax::NodeEditor::PinId(Gen()), ax::NodeEditor::PinKind::Output, &producer, &slag));

    REQUIRE(NodeProductionHidden(producer, { "Iron Ingot" }) == false);          // Slag still visible
    REQUIRE(NodeProductionHidden(producer, { "Iron Ingot", "Slag" }) == true);   // all hidden
}
```

Add the needed includes at the top of `test_node_display.cpp` if missing:
```cpp
#include <set>
#include "domain/graph/pin.hpp"
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Release --target fc-tests`
Expected: compile error — `NodeProductionHidden` is not declared.

- [ ] **Step 3: Declare the function**

In `node_display.hpp`, add `#include <set>` near the top includes, then add after the `NodesConsumingItem` declaration (after line 42):

```cpp
/// @brief True when `node` is a producer (Craft or Extractor) that has at least
/// one output item AND every one of its output items is present in `hidden_items`.
/// Non-producers, and producers with any still-visible output item, return false.
/// Used to pure-hide a production from the read-only snapshot canvas. Pure: no UI.
bool NodeProductionHidden(const Node& node, const std::set<std::string>& hidden_items);
```

- [ ] **Step 4: Implement the function**

In `node_display.cpp`, add `#include <set>` near the top includes if not present, then add after `NodesConsumingItem`'s definition:

```cpp
bool NodeProductionHidden(const Node& node, const std::set<std::string>& hidden_items)
{
    if (!node.IsCraft() && !node.IsExtractor()) return false;
    bool has_output_item = false;
    for (const auto& pin : node.outs)
    {
        if (!pin || !pin->item) continue;
        has_output_item = true;
        if (hidden_items.find(pin->item->name) == hidden_items.end())
            return false; // an output item is still visible => keep the node
    }
    return has_output_item; // producer with at least one item, all hidden
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests` then
`build\ficsit-companion\Release\fc-tests.exe "[node_display]"`
Expected: PASS (all `[node_display]` cases, including the two new ones).

- [ ] **Step 6: Mark Task 2 complete in the PROGRESS file.**

---

### Task 3: Drop-vs-bypass in `ComputeVisibleEdges`

**Files:**
- Modify: `ficsit-companion/include/domain/snapshot/snapshot_bypass.hpp`
- Modify: `ficsit-companion/src/domain/snapshot/snapshot_bypass.cpp`
- Test: `ficsit-companion/tests/test_snapshot_bypass.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces: a 4-arg overload of `ComputeVisibleEdges` whose new last parameter `should_bypass` (defaulted to "always true") decides, for a hidden node, whether to reroute through it (true) or drop it and its links (false):

```cpp
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden,
    const std::function<bool(const Node&)>& should_bypass = [](const Node&) { return true; });
```

- [ ] **Step 1: Write the failing tests**

The existing `BypassFixture::Run()` calls the 3-arg form; keep it. Add a drop-aware runner and tests to `ficsit-companion/tests/test_snapshot_bypass.cpp`. Inside `struct BypassFixture`, add a second hidden set and runner:

```cpp
        std::unordered_set<const Node*> dropped; // hidden AND not bypassed
        void Drop(const Node* n) { hidden.insert(n); dropped.insert(n); }

        std::vector<SnapshotEdge> RunWithDrop()
        {
            return ComputeVisibleEdges(nodes, links,
                [this](const Node& n) { return hidden.count(&n) != 0; },
                [this](const Node& n) { return dropped.count(&n) == 0; }); // bypass unless dropped
        }
```

Then add these test cases at the end of the file:

```cpp
/// @test A dropped node emits no edges: both its inbound and outbound links vanish.
TEST_CASE("dropped node removes its links entirely", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    MergerNode* d = fx.AddMerger(); fx.Drop(d);     // dropped, not bypassed
    CustomSplitterNode* c = fx.AddSplitter();
    fx.Connect(p->outs[0].get(), d->ins[0].get());  // p -> d
    fx.Connect(d->outs[0].get(), c->ins[0].get());  // d -> c

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.empty()); // no p->c edge; the dropped node's links disappear
}

/// @test A bypassed node still reroutes when another node is merely dropped.
TEST_CASE("drop predicate leaves bypass nodes rerouting", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    CustomSplitterNode* b = fx.AddSplitter(); fx.Hide(b);   // bypassed (hidden, not dropped)
    MergerNode* c = fx.AddMerger();
    fx.Connect(p->outs[0].get(), b->ins[0].get());
    fx.Connect(b->outs[0].get(), c->ins[0].get());

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.size() == 1);
    REQUIRE(BypassFixture::Has(edges, p->outs[0].get(), c->ins[0].get()));
}

/// @test A chain visible -> bypass -> drop -> consumer terminates at the drop.
TEST_CASE("bypass chain terminates at a dropped node", "[snapshot_bypass]")
{
    BypassFixture fx;
    CustomSplitterNode* p = fx.AddSplitter();
    CustomSplitterNode* b = fx.AddSplitter(); fx.Hide(b);   // bypassed
    MergerNode* d = fx.AddMerger(); fx.Drop(d);             // dropped
    CustomSplitterNode* c = fx.AddSplitter();
    fx.Connect(p->outs[0].get(), b->ins[0].get());
    fx.Connect(b->outs[0].get(), d->ins[0].get());
    fx.Connect(d->outs[0].get(), c->ins[0].get());

    auto edges = fx.RunWithDrop();
    REQUIRE(edges.empty()); // walk reaches d, which is dropped => no edge to c
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Release --target fc-tests`
Expected: compile error — `ComputeVisibleEdges` takes 3 args, not 4.

- [ ] **Step 3: Add the parameter to the header**

Replace the `ComputeVisibleEdges` declaration in `snapshot_bypass.hpp` (lines 29-32) with:

```cpp
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden,
    const std::function<bool(const Node&)>& should_bypass = [](const Node&) { return true; });
```

Also update the doc comment above it: add a sentence — "When a hidden node's `should_bypass` is false, it is *dropped*: the walk emits nothing and stops, so the node and its links disappear instead of being rerouted."

- [ ] **Step 4: Implement drop handling**

In `snapshot_bypass.cpp`, thread `should_bypass` through both the recursion helper and the main function.

Replace `CollectVisibleSinks` (lines 15-31) with:

```cpp
    void CollectVisibleSinks(const Node& hidden_node,
                             const std::function<bool(const Node&)>& is_hidden,
                             const std::function<bool(const Node&)>& should_bypass,
                             std::unordered_set<const Node*>& visited,
                             std::vector<ax::NodeEditor::PinId>& sinks)
    {
        if (!should_bypass(hidden_node)) return; // dropped: stop, emit nothing
        if (!visited.insert(&hidden_node).second) return;
        for (const auto& out : hidden_node.outs)
        {
            if (!out || !out->link) continue;
            const Pin* end = out->link->end;
            if (!end || !end->node) continue;
            if (is_hidden(*end->node))
                CollectVisibleSinks(*end->node, is_hidden, should_bypass, visited, sinks);
            else
                sinks.push_back(end->id);
        }
    }
```

Replace the `ComputeVisibleEdges` signature line (lines 34-37) with:

```cpp
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& /*nodes*/,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden,
    const std::function<bool(const Node&)>& should_bypass)
```

And update the recursion call inside the `else` branch (line 64) to pass `should_bypass`:

```cpp
            CollectVisibleSinks(*end->node, is_hidden, should_bypass, visited, sinks);
```

(The existing `if (is_hidden(*start->node)) continue;` already skips links *starting* at a hidden node, so a dropped node's outbound links are never emitted from this loop; the new guard in `CollectVisibleSinks` handles the case where a dropped node is reached via the forward walk.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests` then
`build\ficsit-companion\Release\fc-tests.exe "[snapshot_bypass]"`
Expected: PASS — all prior `[snapshot_bypass]` cases (default overload unchanged) plus the three new drop cases.

- [ ] **Step 6: Mark Task 3 complete in the PROGRESS file.**

---

### Task 4: Wire the production filter into the app's hide logic

**Files:**
- Modify: `ficsit-companion/include/app/factory_snapshot_app.hpp`
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp`

**Interfaces:**
- Consumes: `FactorySnapshotSession::hidden_production_items` (Task 1), `NodeProductionHidden` (Task 2), the 4-arg `ComputeVisibleEdges` (Task 3).
- Produces: `FactorySnapshotApp::hidden_production_set` (a `std::set<std::string>` cache) and `void RebuildHiddenProductionSet();`, plus updated `IsNodeHidden`/`RebuildVisibleEdges` so production-hidden nodes are dropped and logistics-hidden nodes still bypass.

This task has no unit test (it lives in the app/UI layer, exercised by the domain tests of Tasks 2-3 and manual verification in Task 5). The deliverable is a clean build.

- [ ] **Step 1: Add the cache member + helper declaration**

In `factory_snapshot_app.hpp`, add `#include <set>` near the top includes. Add this declaration in the `private:` section near `RebuildVisibleEdges` (after line 65):

```cpp
    /// @brief Rebuild `hidden_production_set` from session.hidden_production_items.
    /// Call after loading a session and after any checkbox/select-all change.
    void RebuildHiddenProductionSet();
```

Add this member near `visible_edges` (after line 84):

```cpp
    /// @brief Fast-lookup mirror of session.hidden_production_items; consulted by
    /// IsNodeHidden. Rebuilt whenever that list changes.
    std::set<std::string> hidden_production_set;
```

- [ ] **Step 2: Implement the helper and update IsNodeHidden / RebuildVisibleEdges**

In `factory_snapshot_app.cpp`, add `#include <set>` near the top includes.

Add the helper (place it just above `IsNodeHidden`, before line 682):

```cpp
void FactorySnapshotApp::RebuildHiddenProductionSet()
{
    hidden_production_set.clear();
    hidden_production_set.insert(session.hidden_production_items.begin(),
                                session.hidden_production_items.end());
}
```

Replace `IsNodeHidden` (lines 682-693) with:

```cpp
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
```

Replace `RebuildVisibleEdges` (lines 695-704) with a version that drops production-hidden nodes while still bypassing logistics-hidden ones:

```cpp
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
```

- [ ] **Step 3: Build the set on load**

In `LoadSession()` (lines 176-185), after `session.Deserialize(ss.str());`, add a call so a restored hidden list takes effect immediately. Because `LoadSession()` is `#if !defined(__EMSCRIPTEN__)`-guarded, place the call *outside* the guard at the end of the function so the set is always consistent:

Change `LoadSession` to:

```cpp
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
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build --config Release --target fc-tests`
Expected: builds cleanly (the test target links the app sources). No new tests, but all existing tests must still pass:
`ctest --test-dir build -C Release --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Mark Task 4 complete in the PROGRESS file.**

---

### Task 5: Resources-tab checkbox column + Select/Unselect all

**Files:**
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp` (`RenderResourceFlowTable`, lines 369-440)

**Interfaces:**
- Consumes: `session.hidden_production_items`, `RebuildHiddenProductionSet()`, `RebuildVisibleEdges()`, `SaveSession()`.
- Produces: UI only — a "Show" checkbox column and two buttons. No new public interface.

This is UI; verification is manual (run the app, import a save, toggle items). There is no Catch2 test for ImGui rendering.

- [ ] **Step 1: Add Select all / Unselect all buttons**

In `RenderResourceFlowTable`, after the search `InputText` block (after line 382, before `const ResourceFlowFilter flow_filter = ...`), add:

```cpp
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
```

Ensure `#include <set>` is present (added in Task 4) and that `domain/core/fractional_number.hpp` is available — it is already pulled in transitively via `resource_flow.hpp`.

- [ ] **Step 2: Grow the table to 5 columns with a leading "Show" column**

Change the `BeginTable` call (line 387) from `4` columns to `5`:

```cpp
    if (ImGui::BeginTable("##flow_table", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
        ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
```

Add the new column setup as the first column, before `TableSetupColumn("Item")` (line 391):

```cpp
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 36.0f);
```

- [ ] **Step 3: Render the checkbox cell per row**

Inside the row loop, immediately after `ImGui::PushID(static_cast<const void*>(row.item));` (line 403) and before the existing first `ImGui::TableNextColumn();` (line 405), insert the Show column:

```cpp
            ImGui::TableNextColumn();
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
```

(`<algorithm>` for `std::remove` is already included at the top of the file, line 6.)

- [ ] **Step 4: Build**

Run: `cmake --build build --config Release --target fc-tests`
Expected: builds cleanly. Then run the full app build:
`cmake --build build --config Release`
Expected: builds cleanly.

- [ ] **Step 5: Manual verification**

Launch the app, import a `.sav` (or load an existing session), open the **Resources** tab. Confirm:
- Every produced item has a checked "Show" box; consumed-only rows have no checkbox.
- Unchecking an item hides its producing machines on the canvas and their links disappear (no rerouted edge); the Produced/Consumed/Net numbers are unchanged.
- "Unselect all" hides all productions; "Select all" restores them.
- Re-launching the app preserves the checkbox state (persisted).

- [ ] **Step 6: Mark Task 5 complete in the PROGRESS file.**

---

## Notes for the implementer

- Run the full suite once at the end: `ctest --test-dir build -C Release --output-on-failure`.
- `FractionalNumber` comparison: `row.produced != FractionalNumber(0, 1)` tests "is produced". `FractionalNumber` supports `==`/`!=` (used elsewhere in the codebase); if the exact constructor differs, compare against a default-constructed `FractionalNumber{}` which the header documents as `0/1`.
- Do not touch the Topology summary or the Produced/Consumed/Net values — the filter is visualization-only.
