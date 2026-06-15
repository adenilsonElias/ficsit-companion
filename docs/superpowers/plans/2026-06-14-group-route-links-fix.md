# Fix: Vehicle Route Links Lost on Group Serialize / Ungroup — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. TDD throughout.

**Goal:** Vehicle-station route links (plug↔plug connections) survive `GroupNode` serialize/deserialize and ungrouping, so imported routes are no longer lost on save→reload or ungroup.

**Tech Stack:** C++17, Catch2, CMake/MSVC. No git (user preference) — run the full suite + tick boxes instead of committing.

---

## Root cause (confirmed via `build/ficsit-companion/Release/saved/debug.fcs`)

Route links are only persisted by `SessionSerializer` (top-level), which writes a separate
`route_links` array and rebuilds them through each station's plug
(`session_serializer.cpp:62-89, 167-195`). The **group machinery has no concept of route links**:

1. `ProductionApp::ImportSavFromJson` (`production_app.cpp:3631`) wraps imported nodes + route
   links into a `GroupNode`. Connected at this point.
2. `GroupNode::Serialize` (`group_node.cpp:144-166`) computes each inner link's pin index via
   `get_pin_index`, which searches only `ins`/`outs`. **A plug pin lives outside `ins`/`outs`, so
   it returns `-1` and the link is skipped** (`continue`, line 153). Route links are dropped.
   `GroupNode::Deserialize` (`group_node.cpp:53-80`) likewise only rebuilds `outs`/`ins` pin links.
3. `ProductionApp::UngroupSelectedNode` (`production_app.cpp:436, 481-505`) rebuilds the canvas
   from `group->Serialize()` (already missing the route links) and only reconstructs pin links.

Evidence in `debug.fcs`: 46 `VehicleStationNode`s with modes intact, **no group node** (it was
ungrouped), and top-level **`"route_links":[]`** — every route lost.

**Fix:** mirror `SessionSerializer`'s route-link handling in the three group spots.

---

## File Structure

| File | Change |
|------|--------|
| `ficsit-companion/src/domain/group_node.cpp` | Serialize + Deserialize plug↔plug route links. |
| `ficsit-companion/src/app/production_app.cpp` | Reconstruct route links in `UngroupSelectedNode`. |
| `ficsit-companion/tests/test_node_serialization.cpp` | Group round-trip test (route links survive). |
| `ficsit-companion/tests/test_*` (ungroup) | Ungroup test if an ungroup-testable seam exists; otherwise cover via the GroupNode round-trip + a manual note. |

Shared helper: a `station_plug(const Node*)` returning a Truck/Train station's `plug` (copy the
lambda from `session_serializer.cpp:168-174`). `IsVehiclePlug` comes from
`domain/graph_item_resolve.hpp` (add the include to `group_node.cpp`).

---

## Task 1: `GroupNode::Serialize` emits route links

**Files:**
- Modify: `ficsit-companion/src/domain/group_node.cpp` (add `#include "domain/graph_item_resolve.hpp"`; the link loop ~144-166; add a `route_links` array after `node["links"] = serialized_links;` at line 167)
- Test: `ficsit-companion/tests/test_node_serialization.cpp`

- [x] **Step 1: Write the failing test.** Build a `GroupNode` holding two `VehicleStationNode`s
  (one Load, one Unload) plus a plug↔plug route `Link` recorded in both stations' `route_links`
  and in the group's `links`. Serialize, then deserialize into a new `GroupNode`, and assert the
  rebuilt stations each have `route_links.size() == 1` referencing the same `Link`, with both
  plugs' `Pin::link == nullptr`.

  ```cpp
  /// @test A GroupNode containing a plug<->plug vehicle route link round-trips
  ///       through Serialize/Deserialize with the route link preserved.
  /// @covers GroupNode route-link serialization.
  TEST_CASE("GroupNode round-trips vehicle route links", "[group][vehicle_route]")
  {
      // ... construct loader (Load) + unloader (Unload) VehicleStationNode,
      // a Link(plug_out -> plug_in) pushed into both route_links and group links,
      // wrap in GroupNode, Serialize() -> Deserialize() -> assert route_links survive.
  }
  ```
  (Use the existing test helpers / `IdGen`; mirror `LinkPlugs` from `test_vehicle_station_node.cpp`.)

- [x] **Step 2:** Build + run; expect FAIL (deserialized stations have empty `route_links`).
  `cmake --build build --config Release --target fc-tests`
  `ctest --test-dir build -C Release -R "round-trips vehicle route links" --output-on-failure`

- [x] **Step 3: Implement.** In `group_node.cpp`, add the include, then after the existing link
  loop add a route-link array (skip plug links in the *regular* loop so they aren't emitted as
  `-1` pin links, then emit them separately):

  In the regular link loop (line ~144), skip route links:
  ```cpp
      for (const auto& l : links)
      {
          if (IsVehiclePlug(l->start) && IsVehiclePlug(l->end)) continue; // route link, handled below
          const int start_node_index = get_node_index(l->start->node);
          // ... unchanged ...
      }
  ```
  After `node["links"] = serialized_links;`:
  ```cpp
      // Route links (plug<->plug) are serialized by node indices only, like
      // SessionSerializer, because plugs live outside ins/outs.
      Json::Array serialized_route_links;
      for (const auto& l : links)
      {
          if (!IsVehiclePlug(l->start) || !IsVehiclePlug(l->end)) continue;
          const int s = get_node_index(l->start->node);
          const int e = get_node_index(l->end->node);
          if (s == -1 || e == -1) continue;
          serialized_route_links.push_back({ { "start", s }, { "end", e } });
      }
      node["route_links"] = serialized_route_links;
  ```

- [x] **Step 4:** (test still fails until Task 2 rebuilds them) — proceed to Task 2, then re-run.

---

## Task 2: `GroupNode::Deserialize` rebuilds route links

**Files:**
- Modify: `ficsit-companion/src/domain/group_node.cpp` (after the existing link-rebuild loop ends at line ~80, before `CreateInsOuts`)

- [x] **Step 1:** (covered by Task 1's test — it asserts the rebuilt state).

- [x] **Step 2: Implement.** After the `serialized["links"]` loop (line ~80), add:

  ```cpp
      // Rebuild route (plug<->plug) links between vehicle stations.
      auto station_plug = [](const Node* n) -> Pin* {
          if (n == nullptr || !n->IsLogistics()) return nullptr;
          auto* lg = static_cast<const LogisticsNode*>(n);
          if (lg->logistics_kind != LogisticsNode::Kind::TruckStation &&
              lg->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
          return static_cast<const VehicleStationNode*>(lg)->plug.get();
      };
      if (serialized.contains("route_links"))
      {
          for (const auto& l : serialized["route_links"].get_array())
          {
              const int s = l["start"].get<int>();
              const int e = l["end"].get<int>();
              if (s < 0 || e < 0 || s >= node_indices.size() || e >= node_indices.size() ||
                  node_indices[s] == -1 || node_indices[e] == -1)
              {
                  loading_error = true;
                  continue;
              }
              Pin* start_plug = station_plug(nodes[node_indices[s]].get());
              Pin* end_plug = station_plug(nodes[node_indices[e]].get());
              if (start_plug == nullptr || end_plug == nullptr) { loading_error = true; continue; }
              links.emplace_back(std::make_unique<Link>(local_id_generator(), start_plug, end_plug));
              Link* rl = links.back().get();
              static_cast<VehicleStationNode*>(start_plug->node)->route_links.push_back(rl);
              static_cast<VehicleStationNode*>(end_plug->node)->route_links.push_back(rl);
              // Pin::link stays null on plugs (route links tracked in route_links).
          }
      }
  ```
  Note: do NOT set `start_plug->link` / `end_plug->link` (plugs keep `Pin::link == nullptr`).

- [x] **Step 3:** Build + run the Task 1 test; expect PASS.
  `cmake --build build --config Release --target fc-tests`
  `ctest --test-dir build -C Release -R "round-trips vehicle route links" --output-on-failure`

- [x] **Step 4:** Run full suite; expect all pass.

---

## Task 3: `UngroupSelectedNode` reconstructs route links

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp` (`UngroupSelectedNode`, after the link-rebuild loop ~505, before the extractor post-pass ~519)

- [x] **Step 1: Write a failing test if an ungroup seam is testable.** `UngroupSelectedNode`
  depends on ImGui selection state, so a pure unit test may not be feasible. If there is no
  headless seam, document this and rely on: (a) the GroupNode round-trip test from Task 1 (which
  proves `serialized["route_links"]` is emitted/consumed), and (b) manual verification (import a
  save with routes, ungroup, confirm stations stay connected). Prefer extracting the route-link
  reconstruction into a small testable helper if practical.
  Result: no clean headless seam exists because the method is private and selected via live
  ImGui NodeEditor state. Coverage relies on the GroupNode route-link round-trip test plus the
  existing `GraphModel::CreateLink leaves restored state untouched in restore mode` test for the
  plug-link restore path used by ungroup.

- [x] **Step 2: Implement.** After the `serialized["links"]` reconstruction loop (line ~505),
  using the same `serialized_index_to_main_index` map, add:

  ```cpp
      // Reconstruct vehicle route (plug<->plug) links so ungrouping preserves them.
      auto station_plug = [](Node* n) -> Pin* {
          if (n == nullptr || !n->IsLogistics()) return nullptr;
          auto* lg = static_cast<LogisticsNode*>(n);
          if (lg->logistics_kind != LogisticsNode::Kind::TruckStation &&
              lg->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
          return static_cast<VehicleStationNode*>(lg)->plug.get();
      };
      if (serialized.contains("route_links"))
      {
          for (const auto& l : serialized["route_links"].get_array())
          {
              const int s = l["start"].get<int>();
              const int e = l["end"].get<int>();
              if (s < 0 || e < 0 || s >= static_cast<int>(serialized_index_to_main_index.size()) ||
                  e >= static_cast<int>(serialized_index_to_main_index.size()) ||
                  serialized_index_to_main_index[s] == -1 || serialized_index_to_main_index[e] == -1)
              {
                  links_skipped += 1;
                  continue;
              }
              Pin* start_plug = station_plug(nodes[serialized_index_to_main_index[s]].get());
              Pin* end_plug = station_plug(nodes[serialized_index_to_main_index[e]].get());
              if (start_plug == nullptr || end_plug == nullptr) { links_skipped += 1; continue; }
              CreateLink(start_plug, end_plug, false); // graph_model handles route_links bookkeeping
          }
      }
  ```
  `CreateLink(plug, plug, false)` routes through `GraphModel::CreateLink`'s plug branch
  (`graph_model.cpp:121-149`), which records the link in both stations' `route_links` and leaves
  `Pin::link` null — exactly the restore-mode behavior used by session load.

- [x] **Step 3:** Build the app + run full suite.
  `cmake --build build --config Release`
  `ctest --test-dir build -C Release --output-on-failure`

- [ ] **Step 4: Manual verification.** Import `debug.fcs`'s source save with "Connect vehicle
  routes" enabled, ungroup the imported group, save, reload — confirm the stations remain
  connected (route links present). Also confirm the keep-as-group save/reload path works.

---

## Notes / scope

- The inverse path (selecting stations and **creating** a group via Ctrl+G) must also capture
  route links into the new group's `links`. If `ProductionApp`'s group-creation path builds the
  GroupNode from selected nodes' links, verify it includes plug↔plug links; if it filters by
  `Pin::link`, route links would be missed there too. Check `CreateGroup`/grouping code; add a
  follow-up task if it has the same gap. (Out of scope for the reported bug, which is import→
  ungroup→save.)
- Backward compatibility: the `serialized.contains("route_links")` guards keep old group saves
  (without the array) loading cleanly.
- Verify `domain/graph_item_resolve.hpp` (for `IsVehiclePlug`) does not introduce an include cycle
  in `group_node.cpp` (it only forward-declares and includes `<functional>` — safe).
