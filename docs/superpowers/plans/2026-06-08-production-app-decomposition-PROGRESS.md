# ProductionApp Decomposition — EXECUTION PROGRESS / RESUME CHECKPOINT

Last updated: 2026-06-08. Read this first when resuming.

Latest checkpoint: T6 GraphModel, T7 SessionSerializer, and T8 sav world-name
derivation are DONE. T9 final verification is NEXT. Normal repo-root commands
work in the updated environment: `cmake --build build --config Release --target
fc-tests`, `ctest --test-dir build -C Release --output-on-failure`, and
`cmake --build build --config Release --target ficsit-companion`. Current
verified count: 17/17 tests pass; app target builds. T9 automated verification
also ran `cmake --build build --config Release --clean-first` and then `ctest
--test-dir build -C Release --output-on-failure`; both passed. Manual GUI smoke
was not launched from Codex.

## What this is
Executing `docs/superpowers/plans/2026-06-08-production-app-decomposition.md`
(spec: `...specs/2026-06-08-production-app-decomposition-design.md`) via
subagent-driven development.

## Execution ground rules (decided with user)
- Work directly on `main`. **NO git commits / adds** — the user manages git
  themselves. Every "Commit" step in the plan is SKIPPED.
- Build/configure from the **repo ROOT**: `cmake -DCMAKE_BUILD_TYPE=Release -S . -B build`
  then `cmake --build build --config Release`. Test: `ctest --test-dir build -C Release --output-on-failure`.
- Windows / PowerShell.
- The working tree has substantial **pre-existing uncommitted WIP** (Extractor &
  Logistics node kinds, sav import, vehicle map). Verbatim "moves" must use the
  **working-tree file as the baseline, NOT HEAD**. A reviewer diffing against
  HEAD will produce FALSE POSITIVES (this already happened in T5 review).

## Status
| Task | State | Notes |
|---|---|---|
| T1 fc-core lib + Catch2 fc-tests | ✅ DONE | also hardened test linking (see gotchas) |
| T2 IFileStore | ✅ DONE | 7 call sites rewired |
| T3 SettingsStore | ✅ DONE | Settings struct moved out (13 fields), 17 SaveSettings sites |
| T4 IEditorBackend | ✅ DONE | 5-method interface; audit confirms it covers all non-render ax::NodeEditor calls |
| T5 RateSolver | ✅ DONE | verbatim move proven; 5 tests (merger/splitter/chain/locked/error-path) |
| **T6 GraphModel** | ⏳ NEXT (not started) | see detailed instructions below |
| T7 SessionSerializer | pending | |
| T8 sav world-name derivation | pending | |
| T9 final verification | pending | |

**11 tests currently pass.** App (`ficsit-companion.exe`) builds.

## Files created so far
- `cmake/catch2.cmake`; root `CMakeLists.txt` (enable_testing + include catch2, web-guarded)
- `cmake/imgui.cmake` — split into `imgui_core_SOURCE` (9 non-backend sources incl. node-editor) and `imgui_SOURCE` (= core + 2 backends)
- `ficsit-companion/CMakeLists.txt` — `fc-core` STATIC lib + `ficsit-companion` exe + `fc-tests` exe
- `include/file_store.hpp` + `src/file_store.cpp` (IFileStore/Disk/Web/InMemory)
- `include/settings_store.hpp` + `src/settings_store.cpp` (Settings struct + SettingsStore)
- `include/editor_backend.hpp` + `src/editor_backend.cpp` (IEditorBackend/NodeEditorBackend)
- `include/rate_solver.hpp` + `src/rate_solver.cpp` (RateSolver::Solve)
- `tests/test_smoke.cpp, test_graph_types_link.cpp, test_file_store.cpp, test_settings_store.cpp, test_editor_backend.cpp, test_rate_solver.cpp`
- `tests/graph_test_helpers.hpp` (FakeEditorBackend, IdGen, MakeLink)

## CRITICAL gotchas / deviations from the written plan
1. **fc-core stays "pure".** `utils.cpp` is in the APP target (it includes
   `<SDL_opengl.h>`). `fc-tests` ALSO compiles `utils.cpp` + links `${OPENGL_LIBRARIES}`
   + `SDL2::SDL2-static` + `${imgui_core_SOURCE}`, because fc-core types
   (recipe.cpp→`LoadTextureFromFile`, fractional_number.cpp→ImGui) need those
   symbols at test link time. New testable `.cpp` go in `CORE_SOURCE_FILES`;
   new tests go in `TEST_SOURCE_FILES` (comment markers exist in the CMakeLists).
2. **Item construction in tests:** pass an **empty icon path** or use `nullptr`
   items — a real path triggers `glGenTextures` (crash, no GL context).
3. **RateSolver::Solve signature deviates from the plan.** Actual:
   `static bool Solve(std::vector<std::unique_ptr<Node>>& nodes, std::vector<std::unique_ptr<Link>>& links, const Pin* constraint_pin, const FractionalNumber& constraint_value, float& error_time, float error_flow_duration);`
   `error_time` is reset/set/checked INSIDE Solve (it was an internal accumulator);
   the 4 `error_time = ax::NodeEditor::GetStyle().FlowDuration;` became
   `error_time = error_flow_duration;`. No IEditorBackend needed in RateSolver.
   Wrapper: `ProductionApp::UpdateNodesRate` →
   `return RateSolver::Solve(nodes, links, constraint_pin, constraint_value, error_time, ax::NodeEditor::GetStyle().FlowDuration);`
4. Review process: controller-side review for mechanical tasks (T2/T3/T4/T8);
   dedicated reviewer subagent for big logic moves (T5 done, do same for T6/T7).

## >>> T6 (GraphModel) — DETAILED INSTRUCTIONS FOR NEXT SESSION <<<
Goal: move `nodes`, `links`, `next_id`, `GetNextId`, `FindPin`, `CreateLink`,
`DeleteLink`, `DeleteNode` into a `GraphModel`, plus a shared
`graph_item_resolve` module. Keep all rendering/node-op code compiling via
reference-member binding + delegating wrappers.

Current line numbers in `src/production_app.cpp` (WILL shift — locate by name):
- `IsFuelPin` def L58; `ResolveItemThroughChain` L317; `ResolveOrganizerItem` L357;
  `RecalculateOrganizerItemChain` L402; `RenderOrganizerRecalcButton` L449 (STAYS);
  `RenderOrganizerRecalcChainButton` L468 (STAYS); `PropagateExtractorResourceUpstream` L484.
- `GetNextId` L280; `FindPin` L285; `CreateLink` L520; `DeleteLink` L635; `DeleteNode` L678;
  `UpdateNodesRate` wrapper L701 (already a wrapper).
- `next_id` direct uses: L79 (`next_id = 1;` in ctor) and L282 (`return next_id++;`).
- `production_app.hpp` member decls: `nodes` L159, `links` L161, `config` L163,
  `context` L164, `next_id` L167.

### Step A — shared `graph_item_resolve` (fc-core)
Create `include/graph_item_resolve.hpp` + `src/graph_item_resolve.cpp`. Move
these 5 helpers OUT of production_app.cpp (make them external-linkage, declared
in the header), VERBATIM bodies:
`bool IsFuelPin(const Pin*)`, `const Item* ResolveItemThroughChain(Pin*)`,
`const Item* ResolveOrganizerItem(Node*)`, `void RecalculateOrganizerItemChain(OrganizerNode*)`,
`void PropagateExtractorResourceUpstream(Node*, const Item*, const std::function<unsigned long long int()>&)`.
Add `src/graph_item_resolve.cpp` to `CORE_SOURCE_FILES`. `#include "graph_item_resolve.hpp"`
in BOTH `graph_model.cpp` and `production_app.cpp`. Remove the now-moved static
defs from production_app.cpp. KEEP `RenderOrganizerRecalcButton` /
`RenderOrganizerRecalcChainButton` in production_app.cpp (they call the shared
ResolveOrganizerItem / RecalculateOrganizerItemChain).
NOTE: these helpers reference `OrganizerNode`/`ExtractorNode`/`LogisticsNode` →
include `node.hpp`, `pin.hpp`, `link.hpp`, `recipe.hpp`, `<unordered_set>`,
`<vector>`, `<functional>`.

### Step B — `GraphModel` (fc-core)
`include/graph_model.hpp`:
```cpp
#pragma once
#include <memory>
#include <vector>
#include <imgui_node_editor.h>
#include "fractional_number.hpp"
struct Link; struct Node; struct Pin; class IEditorBackend;
class GraphModel {
public:
    explicit GraphModel(IEditorBackend& editor);
    unsigned long long int GetNextId();
    Pin* FindPin(ax::NodeEditor::PinId id) const;
    // start/end any order; if trigger_update, runs RateSolver and undoes the link if rejected.
    void CreateLink(Pin* start, Pin* end, bool trigger_update, float& error_time, float error_flow_duration);
    void DeleteLink(ax::NodeEditor::LinkId id);
    void DeleteNode(ax::NodeEditor::NodeId id);
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    unsigned long long int next_id = 1;
private:
    IEditorBackend& editor;
};
```
`src/graph_model.cpp`: move the 5 method bodies VERBATIM with substitutions:
- `ax::NodeEditor::DeleteLink(id)` → `editor.DeleteLink(id)`; `ax::NodeEditor::DeleteNode(id)` → `editor.DeleteNode(id)`.
- Inside CreateLink: `if (!UpdateNodesRate(start, start->current_rate))` →
  `if (!RateSolver::Solve(nodes, links, start, start->current_rate, error_time, error_flow_duration))`.
  The catch path `error_time = ax::NodeEditor::GetStyle().FlowDuration;` → `error_time = error_flow_duration;`.
- `std::bind(&ProductionApp::GetNextId, this)` (the 2 inside CreateLink) → `[this]{ return GetNextId(); }`.
- Add `src/graph_model.cpp` to CORE_SOURCE_FILES. Includes: rate_solver.hpp,
  editor_backend.hpp, graph_item_resolve.hpp, node.hpp, pin.hpp, link.hpp,
  recipe.hpp, building.hpp, `<algorithm>`.

### Step C — rewire ProductionApp (reference-member binding)
`production_app.hpp`: add `#include "graph_model.hpp"`, `#include "editor_backend.hpp"`.
Replace the `nodes`/`links`/`next_id` member decls with (ORDER MATTERS):
```cpp
std::unique_ptr<IEditorBackend> editor_backend;   // declared BEFORE graph
GraphModel graph;                                 // needs *editor_backend
std::vector<std::unique_ptr<Node>>& nodes = graph.nodes;   // refs AFTER graph
std::vector<std::unique_ptr<Link>>& links = graph.links;
```
Keep declarations of GetNextId/FindPin/CreateLink/DeleteLink/DeleteNode (wrappers).
`production_app.cpp` constructor — use an init list (members init in decl order):
```cpp
ProductionApp::ProductionApp()
    : editor_backend(std::make_unique<NodeEditorBackend>())
    , graph(*editor_backend)
{
    // file_store init stays; REMOVE the `next_id = 1;` line (now GraphModel default).
    ...
}
```
Delete the moved GetNextId/FindPin/CreateLink/DeleteLink/DeleteNode bodies; add wrappers:
```cpp
unsigned long long int ProductionApp::GetNextId() { return graph.GetNextId(); }
Pin* ProductionApp::FindPin(ax::NodeEditor::PinId id) const { return graph.FindPin(id); }
void ProductionApp::CreateLink(Pin* start, Pin* end, const bool trigger_update) {
    graph.CreateLink(start, end, trigger_update, error_time, ax::NodeEditor::GetStyle().FlowDuration);
}
void ProductionApp::DeleteLink(const ax::NodeEditor::LinkId id) { graph.DeleteLink(id); }
void ProductionApp::DeleteNode(const ax::NodeEditor::NodeId id) { graph.DeleteNode(id); }
```
The many `std::bind(&ProductionApp::GetNextId, this)` calls in render/node-op code
STAY (the wrapper still exists). `next_id` is only used at L79/L282 — both handled above.

### Step D — tests `tests/test_graph_model.cpp` (add to TEST_SOURCE_FILES)
- `GetNextId` increments.
- `DeleteNode` removes the node from `g.nodes`, removes incident links from
  `g.links`, and records the deletion on `FakeEditorBackend` (build 2 nodes +
  a MakeLink between them via `g.nodes`/`g.links`; use the FakeEditorBackend
  passed to the GraphModel ctor).
Build + ctest; then dispatch a reviewer subagent (diff vs WORKING TREE, not HEAD).

## After T6
- T7 SessionSerializer: move Serialize/Deserialize (currently still in
  production_app.cpp ~L200-260) into `SessionSerializer(GraphModel& graph, IEditorBackend& editor, int save_version)`.
  Substitutions: nodes→graph.nodes, links→graph.links, SAVE_VERSION→save_version,
  GetNextId→graph.GetNextId(), bind→`[this]{ return graph.GetNextId(); }`,
  ax::NodeEditor::{DeleteNode,DeleteLink,SetNodePosition}→editor.*,
  `CreateLink(a,b,false)`→`graph.CreateLink(a,b,false, <error_time?>, <flow>)`.
  NOTE: Deserialize's CreateLink uses trigger_update=false so error_time path is
  unused — pass a throwaway `float dummy=0.0f; ` and `0.0f`, OR give GraphModel a
  CreateLink overload without the error params for the no-update case. Decide at
  implementation. Wrappers on ProductionApp delegate. Construct
  `session_serializer` in ctor after `graph`.
- T8: extract pure `DeriveWorldName(std::string stem)` + `DiscoverWorldNames(vector<string>)`
  from `RefreshDiscoveredWorlds`; rewire it to collect stems then call DiscoverWorldNames.
- T9: clean build all 3 targets, full ctest, manual app smoke test, confirm
  production_app.cpp shrank (target ~3000-3300 lines from 5082).
