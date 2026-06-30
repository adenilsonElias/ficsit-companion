# Snapshot: cycle producers/consumers from the Produced/Consumed numbers — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the Factory Snapshot resource-flow table, make the Produced number cycle the graph through producing nodes and the Consumed number cycle through consuming nodes.

**Architecture:** Add a pure domain helper `NodesConsumingItem` mirroring the existing `NodesProducingItem`. Give the snapshot app a navigation "mode" (Produce/Consume) so the deferred jump request (consumed inside the editor context in `RenderGraphCanvas`) picks the right node set. Replace the single all-columns row `Selectable` with per-cell click targets.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, Catch2 for tests, CMake build.

## Global Constraints

- No git commits/branches/worktrees — the user tracks progress in a `*-PROGRESS.md` file, not git. Replace any commit step with a PROGRESS.md update.
- All rates remain `FractionalNumber`; display uses `GetStringFloat()`.
- Domain helpers stay pure: no UI, no editor context.
- Build (desktop): `cmake --build build --config Release --target fc-tests`; test: `ctest --test-dir build -C Release --output-on-failure`.
- `node_display.cpp` and `tests/test_node_display.cpp` are already registered in `ficsit-companion/CMakeLists.txt` (lines 68, 227) — no CMake edits required.

---

### Task 1: `NodesConsumingItem` domain helper

**Files:**
- Modify: `ficsit-companion/include/domain/nodes/node_display.hpp` (add declaration after `NodesProducingItem`, ~line 34)
- Modify: `ficsit-companion/src/domain/nodes/node_display.cpp` (add definition after `NodesProducingItem`, ~line 122)
- Test: `ficsit-companion/tests/test_node_display.cpp` (add a TEST_CASE after the `NodesProducingItem` case, ~line 90)

**Interfaces:**
- Consumes: `Node::IsCraft()`, `Node::IsSink()`, `Node::ins` (each pin has `pin->item->name`).
- Produces: `std::vector<const Node*> NodesConsumingItem(const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name);` — consuming nodes (Craft or Sink) with the item on an input pin, in node order, each node listed once.

- [ ] **Step 1: Write the failing test**

Add to `ficsit-companion/tests/test_node_display.cpp`:

```cpp
/// @test NodesConsumingItem returns only Craft/Sink nodes that take the named
/// item on an input pin; pass-through nodes (mergers) and producers (extractors
/// that only output it) are excluded.
/// @covers NodesConsumingItem matching + consumer filtering.
TEST_CASE("NodesConsumingItem returns only consumers of the item", "[node_display]")
{
    const Item iron("Iron Ore", "", 0);

    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::make_unique<SinkNode>(50, Gen, &iron));     // consumes iron (input pin)
    nodes.push_back(std::make_unique<MergerNode>(51, Gen, &iron));   // carries iron on input, but is pass-through
    nodes.push_back(std::make_unique<ExtractorNode>(52, ExtractorNode::Kind::MinerMk1, &iron, ExtractorNode::Purity::Normal, Gen)); // only outputs iron

    const Node* expected_consumer = nodes[0].get();

    const std::vector<const Node*> consumers = NodesConsumingItem(nodes, "Iron Ore");
    REQUIRE(consumers.size() == 1);
    REQUIRE(consumers[0] == expected_consumer);

    REQUIRE(NodesConsumingItem(nodes, "Copper Ore").empty());
}
```

- [ ] **Step 2: Run the test to verify it fails to compile/link**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `NodesConsumingItem` is undeclared / unresolved external.

- [ ] **Step 3: Add the declaration**

In `ficsit-companion/include/domain/nodes/node_display.hpp`, after the `NodesProducingItem` declaration (after line 34):

```cpp

/// @brief Nodes that CONSUME the named item: Craft or Sink nodes with the item
/// on an input pin, in node order. Pass-through nodes (mergers/splitters/
/// storages/logistics) that merely carry it are excluded. Used to jump the
/// snapshot graph to a consumer when a resource's Consumed value is clicked.
/// Pure: no UI, no editor context.
std::vector<const Node*> NodesConsumingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name);
```

- [ ] **Step 4: Add the definition**

In `ficsit-companion/src/domain/nodes/node_display.cpp`, after the closing brace of `NodesProducingItem` (after line 122):

```cpp

std::vector<const Node*> NodesConsumingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name)
{
    std::vector<const Node*> consumers;
    for (const auto& node : nodes)
    {
        if (!node) continue;
        if (!node->IsCraft() && !node->IsSink()) continue;
        for (const auto& pin : node->ins)
        {
            if (pin && pin->item && pin->item->name == item_name)
            {
                consumers.push_back(node.get());
                break;
            }
        }
    }
    return consumers;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build --config Release --target fc-tests` then
`ctest --test-dir build -C Release --output-on-failure -R node_display`
Expected: PASS — all `[node_display]` cases green, including the new one.

- [ ] **Step 6: Mark progress**

Append a line to `docs/superpowers/plans/2026-06-29-snapshot-cycle-producers-consumers-PROGRESS.md` noting Task 1 complete (helper + test).

---

### Task 2: Wire Produced/Consumed clicks to producer/consumer cycling

**Files:**
- Modify: `ficsit-companion/include/app/factory_snapshot_app.hpp` (nav state: ~lines 91-98)
- Modify: `ficsit-companion/src/app/factory_snapshot_app.cpp` — `RenderResourceFlowTable` (lines 397-415) and `RenderGraphCanvas` (lines 460-476)

**Interfaces:**
- Consumes: `NodesProducingItem`, `NodesConsumingItem` from Task 1.
- Produces: no new public symbols; behavioral change only.

- [ ] **Step 1: Add the navigation mode to the header**

In `ficsit-companion/include/app/factory_snapshot_app.hpp`, replace the nav-state block (lines 91-98, the `nav_item` / `nav_cycle_item` / `nav_cycle_index` members and their comments) with:

```cpp
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
```

- [ ] **Step 2: Replace the row Selectable with per-cell click targets**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, replace the body of the `for (ResourceFlowRow& row : model.flow.rows)` loop (lines 397-415) with:

```cpp
        for (ResourceFlowRow& row : model.flow.rows)
        {
            if (!RowPassesFilter(row, flow_filter, session.flow_search)) continue;
            ImGui::TableNextRow();
            // Cell text (item names, rate strings) repeats across rows, so scope
            // the Selectable ids by the row's item pointer to keep them unique.
            ImGui::PushID(static_cast<const void*>(row.item));

            ImGui::TableNextColumn();
            // Item name: jump to a producer (repeated clicks cycle producers).
            // The "##name"/"##prod"/"##cons" suffixes hide a per-cell id so that
            // identical displayed text (e.g. Produced and Consumed both "0") in
            // the same row does not collide.
            const std::string item_name = row.item ? row.item->name : std::string("(unknown)");
            if (ImGui::Selectable((item_name + "##name").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Produce;
            }

            ImGui::TableNextColumn();
            // Produced number: cycle through producing nodes.
            if (ImGui::Selectable((row.produced.GetStringFloat() + "##prod").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Produce;
            }

            ImGui::TableNextColumn();
            // Consumed number: cycle through consuming nodes.
            if (ImGui::Selectable((row.consumed.GetStringFloat() + "##cons").c_str(), false) && row.item)
            {
                nav_item = row.item->name;
                nav_mode = NavMode::Consume;
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.net.GetStringFloat().c_str());

            ImGui::PopID();
        }
```

Note: the `Selectable` labels (`row.produced`/`row.consumed` strings) can be
identical across cells/rows; `PushID(row.item)` plus the distinct column order
keeps ImGui ids unique. If two rows ever shared an item pointer they would also
share a name, which cannot happen (rows are per-item).

- [ ] **Step 3: Make RenderGraphCanvas pick the node set by mode**

In `ficsit-companion/src/app/factory_snapshot_app.cpp`, replace the pending-request block (lines 458-476, from the `// Consume a pending "jump to producer" request` comment through `nav_item.clear();`) with:

```cpp
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
```

- [ ] **Step 4: Build**

Run: `cmake --build build --config Release`
Expected: builds clean (the snapshot app + its test target compile).

- [ ] **Step 5: Run the full test suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS — no regressions; the new `NodesConsumingItem` case is green.

- [ ] **Step 6: Manual verification**

Launch the app, import a `.sav`, open the Resources tab:
- Click an item's **Produced** number repeatedly → the graph selects/centers each producing machine in turn.
- Click the same item's **Consumed** number repeatedly → it cycles through consuming machines/sinks instead (cycle restarts at the first consumer).
- Click the **item name** → still cycles producers.
- Click **Consumed** on a final product nobody consumes → nothing happens (no crash).

- [ ] **Step 7: Mark progress**

Append a line to `docs/superpowers/plans/2026-06-29-snapshot-cycle-producers-consumers-PROGRESS.md` noting Task 2 complete (UI wiring + mode-aware navigation).

---

## Self-Review

- **Spec coverage:**
  - New helper `NodesConsumingItem` → Task 1. ✓
  - Nav mode state (`NavMode`, `nav_mode`, `nav_cycle_mode`, reset rule) → Task 2 Step 1/3. ✓
  - Per-cell clicks (item=producers, Produced=producers, Consumed=consumers, Net inert) → Task 2 Step 2. ✓
  - Mode-aware consume in `RenderGraphCanvas` → Task 2 Step 3. ✓
  - Edge cases (no targets no-op, mode switch resets cycle, id collisions) → Task 2 Steps 2/3 + manual verification. ✓
  - Unit test for `NodesConsumingItem` → Task 1 Step 1. ✓
- **Placeholder scan:** No TBD/TODO; all code shown in full. ✓
- **Type consistency:** `NavMode` enum spelled identically in header and `.cpp`; `NodesConsumingItem` signature identical in header, source, and call site. ✓
