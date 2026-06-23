# Domain Layer Sub-Modules Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize the flat `domain/` layer (20 headers + 31 sources) into seven cohesive sub-module folders, mirrored across `include/` and `src/`, with no logic or layer-boundary changes.

**Architecture:** Pure structural move. Each task relocates one module's files into `include/domain/<module>/` and `src/domain/<module>/`, rewrites every `#include` that references those files to the new module-qualified path, and repoints the matching CMake entries. After each task the build compiles clean and the full Catch2 suite stays green — the existing suite is the safety net (there are no new tests to write for a pure move).

**Tech Stack:** C++17, CMake (FetchContent), Catch2 test suite (`fc-tests`).

## Global Constraints

- **No logic changes.** Every file moves intact; no TU is split, merged, or edited except for its own `#include` lines and (for includers) include paths.
- **No layer-boundary changes.** The 3 known cross-layer violations (`graph_model.cpp → infra/editor_backend.hpp`, `recipe.cpp` & `node.hpp → app/utils.hpp`, `infra/sav_import.cpp → app/utils.hpp`) stay exactly as-is — out of scope.
- **`infra/` and `app/` are untouched** except for `#include` path rewrites where they reference moved domain headers.
- **No git operations** (no commit/branch/worktree). Use plain `mv` for moves; git auto-detects renames from identical content. Track progress in `docs/superpowers/plans/2026-06-22-domain-sub-modules-PROGRESS.md`. Commits happen only if/when the user explicitly asks.
- **Include-rewrite rule:** replace the *exact* string `domain/<file>.hpp` (the `.hpp` boundary prevents prefix collisions, e.g. `domain/node.hpp` never matches `domain/node_data_resolver.hpp`). Scope the rewrite to `ficsit-companion/src`, `ficsit-companion/include`, `ficsit-companion/tests`. **Never** rewrite files under `docs/`.
- **Verification after every module task:**
  - Build: `cmake --build build --config Release --target fc-tests`
  - Test: `ctest --test-dir build -C Release --output-on-failure`
  - Both must succeed before the task is done.
- All shell commands below are run from the repo root `G:\Projetos\satis\ficsit-companion-upgrade` using the Bash tool (Git Bash). Paths are relative to that root.

---

### Task 1: Baseline — establish the green safety net

**Files:** none changed.

- [ ] **Step 1: Configure the build if needed**

Run:
```bash
[ -d build ] || cmake -DCMAKE_BUILD_TYPE=Release -S ficsit-companion -B build
```
Expected: a `build/` directory exists with a generated CMake cache. (If the repo configures from a different source dir, match the existing `build/` setup instead.)

- [ ] **Step 2: Build the test target**

Run: `cmake --build build --config Release --target fc-tests`
Expected: build succeeds.

- [ ] **Step 3: Run the full suite and confirm green**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all tests pass. **Record the test count** (e.g. "NN tests passed") — every later task must match this exact count.

- [ ] **Step 4: Create the PROGRESS file**

Create `docs/superpowers/plans/2026-06-22-domain-sub-modules-PROGRESS.md`:
```markdown
# Domain Sub-Modules — Progress

Baseline: <NN> tests passing.

- [x] Task 1: Baseline green
- [ ] Task 2: core/
- [ ] Task 3: gamedata/
- [ ] Task 4: solver/
- [ ] Task 5: graph/
- [ ] Task 6: nodes/
- [ ] Task 7: vehicle/
- [ ] Task 8: snapshot/
- [ ] Task 9: Final sweep + memory update
```

---

### Task 2: `core/` module (fractional_number, json)

**Files:**
- Move: `include/domain/{fractional_number,json}.hpp` → `include/domain/core/`
- Move: `src/domain/{fractional_number,json}.cpp` → `src/domain/core/`
- Modify: `ficsit-companion/CMakeLists.txt` (HEADER_FILES + DOMAIN_SOURCE_FILES entries for these 4 files)
- Modify: every `src`/`include`/`tests` file that includes `domain/fractional_number.hpp` or `domain/json.hpp`

**Interfaces:** No symbol changes. Only include paths change: `domain/fractional_number.hpp → domain/core/fractional_number.hpp`, `domain/json.hpp → domain/core/json.hpp`.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/core ficsit-companion/src/domain/core
mv ficsit-companion/include/domain/fractional_number.hpp ficsit-companion/include/domain/core/
mv ficsit-companion/include/domain/json.hpp            ficsit-companion/include/domain/core/
mv ficsit-companion/src/domain/fractional_number.cpp   ficsit-companion/src/domain/core/
mv ficsit-companion/src/domain/json.cpp                ficsit-companion/src/domain/core/
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in fractional_number json; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/core/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`, change:
```
	include/domain/fractional_number.hpp
```
to
```
	include/domain/core/fractional_number.hpp
```
and
```
	include/domain/json.hpp
```
to
```
	include/domain/core/json.hpp
```
In `DOMAIN_SOURCE_FILES`, change `src/domain/fractional_number.cpp` → `src/domain/core/fractional_number.cpp` and `src/domain/json.cpp` → `src/domain/core/json.cpp`.

- [ ] **Step 4: Build**

Run: `cmake --build build --config Release --target fc-tests`
Expected: build succeeds. (A "file not found" here means a missed include — fix it and rebuild.)

- [ ] **Step 5: Test**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: same test count as baseline, all green.

- [ ] **Step 6: Mark progress**

Check off Task 2 in the PROGRESS file.

---

### Task 3: `gamedata/` module (building, game_data, recipe)

**Files:**
- Move: `include/domain/{building,game_data,recipe}.hpp` → `include/domain/gamedata/`
- Move: `src/domain/{building,game_data,recipe}.cpp` → `src/domain/gamedata/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: `domain/building.hpp → domain/gamedata/building.hpp`, `domain/game_data.hpp → domain/gamedata/game_data.hpp`, `domain/recipe.hpp → domain/gamedata/recipe.hpp`.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/gamedata ficsit-companion/src/domain/gamedata
for f in building game_data recipe; do
  mv ficsit-companion/include/domain/$f.hpp ficsit-companion/include/domain/gamedata/
  mv ficsit-companion/src/domain/$f.cpp     ficsit-companion/src/domain/gamedata/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in building game_data recipe; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/gamedata/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `include/domain/building.hpp → include/domain/gamedata/building.hpp`, `include/domain/game_data.hpp → include/domain/gamedata/game_data.hpp`, `include/domain/recipe.hpp → include/domain/gamedata/recipe.hpp`.
In `DOMAIN_SOURCE_FILES`: same three `.cpp` paths → `src/domain/gamedata/<f>.cpp`.

- [ ] **Step 4: Build** — `cmake --build build --config Release --target fc-tests` → succeeds.
- [ ] **Step 5: Test** — `ctest --test-dir build -C Release --output-on-failure` → baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 3.

---

### Task 4: `solver/` module (rate_solver + phases, linear_solve)

**Files:**
- Move headers: `include/domain/{rate_solver,linear_solve}.hpp` → `include/domain/solver/`
- Move src-local header: `src/domain/rate_solver_internal.hpp` → `src/domain/solver/`
- Move sources: `src/domain/{rate_solver,rate_solver_apply,rate_solver_seed,rate_solver_system,rate_solver_variables,linear_solve}.cpp` → `src/domain/solver/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: `domain/rate_solver.hpp → domain/solver/rate_solver.hpp`, `domain/linear_solve.hpp → domain/solver/linear_solve.hpp`, and the private `domain/rate_solver_internal.hpp → domain/solver/rate_solver_internal.hpp` (6 call sites: the five `rate_solver*.cpp` plus `tests/test_rate_solver_phases.cpp`). `rate_solver_internal.hpp` is NOT in CMake `HEADER_FILES` — it is a private src header, so no CMake change for it.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/solver ficsit-companion/src/domain/solver
mv ficsit-companion/include/domain/rate_solver.hpp   ficsit-companion/include/domain/solver/
mv ficsit-companion/include/domain/linear_solve.hpp  ficsit-companion/include/domain/solver/
mv ficsit-companion/src/domain/rate_solver_internal.hpp ficsit-companion/src/domain/solver/
for f in rate_solver rate_solver_apply rate_solver_seed rate_solver_system rate_solver_variables linear_solve; do
  mv ficsit-companion/src/domain/$f.cpp ficsit-companion/src/domain/solver/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in rate_solver linear_solve rate_solver_internal; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/solver/$h.hpp#g"
done
```
Note: the order is safe — `domain/rate_solver.hpp` matches only the exact `.hpp`, never `domain/rate_solver_internal.hpp` or `domain/rate_solver_apply…` (those have different suffixes before `.hpp`).

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `include/domain/linear_solve.hpp → include/domain/solver/linear_solve.hpp`, `include/domain/rate_solver.hpp → include/domain/solver/rate_solver.hpp`.
In `DOMAIN_SOURCE_FILES`: `src/domain/linear_solve.cpp`, `src/domain/rate_solver.cpp`, `src/domain/rate_solver_apply.cpp`, `src/domain/rate_solver_seed.cpp`, `src/domain/rate_solver_system.cpp`, `src/domain/rate_solver_variables.cpp` → each prefixed `src/domain/solver/`.

- [ ] **Step 4: Build** — succeeds.
- [ ] **Step 5: Test** — baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 4.

---

### Task 5: `graph/` module (pin, link, graph_model, graph_item_resolve)

**Files:**
- Move: `include/domain/{pin,link,graph_model,graph_item_resolve}.hpp` → `include/domain/graph/`
- Move: `src/domain/{pin,link,graph_model,graph_item_resolve}.cpp` → `src/domain/graph/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: each `domain/<f>.hpp → domain/graph/<f>.hpp` for `pin`, `link`, `graph_model`, `graph_item_resolve`.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/graph ficsit-companion/src/domain/graph
for f in pin link graph_model graph_item_resolve; do
  mv ficsit-companion/include/domain/$f.hpp ficsit-companion/include/domain/graph/
  mv ficsit-companion/src/domain/$f.cpp     ficsit-companion/src/domain/graph/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in pin link graph_model graph_item_resolve; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/graph/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `pin`, `link`, `graph_model`, `graph_item_resolve` headers → `include/domain/graph/<f>.hpp`.
In `DOMAIN_SOURCE_FILES`: the matching four `.cpp` → `src/domain/graph/<f>.cpp`.

- [ ] **Step 4: Build** — succeeds.
- [ ] **Step 5: Test** — baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 5.

---

### Task 6: `nodes/` module (node family + display + resolver)

**Files:**
- Move headers: `include/domain/{node,node_data_resolver,node_display}.hpp` → `include/domain/nodes/`
- Move sources: `src/domain/{node_base,craft_node,group_node,organizer_nodes,sink_node,logistics_node,vehicle_station_node,extractor_node,node_data_resolver,node_display}.cpp` → `src/domain/nodes/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: `domain/node.hpp → domain/nodes/node.hpp`, `domain/node_data_resolver.hpp → domain/nodes/node_data_resolver.hpp`, `domain/node_display.hpp → domain/nodes/node_display.hpp`. The exact-`.hpp` rule keeps `domain/node.hpp` from matching the other two.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/nodes ficsit-companion/src/domain/nodes
for f in node node_data_resolver node_display; do
  mv ficsit-companion/include/domain/$f.hpp ficsit-companion/include/domain/nodes/
done
for f in node_base craft_node group_node organizer_nodes sink_node logistics_node vehicle_station_node extractor_node node_data_resolver node_display; do
  mv ficsit-companion/src/domain/$f.cpp ficsit-companion/src/domain/nodes/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in node node_data_resolver node_display; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/nodes/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `node`, `node_data_resolver`, `node_display` headers → `include/domain/nodes/<f>.hpp`.
In `DOMAIN_SOURCE_FILES`: `node_base`, `craft_node`, `group_node`, `organizer_nodes`, `sink_node`, `logistics_node`, `vehicle_station_node`, `extractor_node`, `node_data_resolver`, `node_display` `.cpp` → `src/domain/nodes/<f>.cpp`.

- [ ] **Step 4: Build** — succeeds.
- [ ] **Step 5: Test** — baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 6.

---

### Task 7: `vehicle/` module (vehicle_map + camera/query, vehicle_route)

**Files:**
- Move: `include/domain/{vehicle_map,vehicle_map_camera,vehicle_map_query,vehicle_route}.hpp` → `include/domain/vehicle/`
- Move: `src/domain/{vehicle_map,vehicle_map_camera,vehicle_map_query,vehicle_route}.cpp` → `src/domain/vehicle/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: each `domain/<f>.hpp → domain/vehicle/<f>.hpp` for `vehicle_map`, `vehicle_map_camera`, `vehicle_map_query`, `vehicle_route`. The exact-`.hpp` rule keeps `domain/vehicle_map.hpp` from matching `domain/vehicle_map_camera.hpp` / `…_query.hpp`.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/vehicle ficsit-companion/src/domain/vehicle
for f in vehicle_map vehicle_map_camera vehicle_map_query vehicle_route; do
  mv ficsit-companion/include/domain/$f.hpp ficsit-companion/include/domain/vehicle/
  mv ficsit-companion/src/domain/$f.cpp     ficsit-companion/src/domain/vehicle/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in vehicle_map vehicle_map_camera vehicle_map_query vehicle_route; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/vehicle/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `vehicle_map`, `vehicle_map_camera`, `vehicle_map_query` headers → `include/domain/vehicle/<f>.hpp` (note: `vehicle_route.hpp` is NOT currently in HEADER_FILES — verify; if absent, add `include/domain/vehicle/vehicle_route.hpp` only if other domain `.hpp` of similar status are listed, otherwise leave HEADER_FILES as the move dictates).
In `DOMAIN_SOURCE_FILES`: `vehicle_map`, `vehicle_map_camera`, `vehicle_map_query`, `vehicle_route` `.cpp` → `src/domain/vehicle/<f>.cpp`.

> Note: confirm against the current `HEADER_FILES` list — only `vehicle_map`, `vehicle_map_camera`, `vehicle_map_query` headers appear there; `vehicle_route.hpp` is not listed. Move the file regardless; only repoint CMake entries that actually exist.

- [ ] **Step 4: Build** — succeeds.
- [ ] **Step 5: Test** — baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 7.

---

### Task 8: `snapshot/` module (factory_snapshot_model, resource_flow)

**Files:**
- Move: `include/domain/{factory_snapshot_model,resource_flow}.hpp` → `include/domain/snapshot/`
- Move: `src/domain/{factory_snapshot_model,resource_flow}.cpp` → `src/domain/snapshot/`
- Modify: `ficsit-companion/CMakeLists.txt` + all includers

**Interfaces:** Include paths only: `domain/factory_snapshot_model.hpp → domain/snapshot/factory_snapshot_model.hpp`, `domain/resource_flow.hpp → domain/snapshot/resource_flow.hpp`.

- [ ] **Step 1: Create folders and move files**

```bash
mkdir -p ficsit-companion/include/domain/snapshot ficsit-companion/src/domain/snapshot
for f in factory_snapshot_model resource_flow; do
  mv ficsit-companion/include/domain/$f.hpp ficsit-companion/include/domain/snapshot/
  mv ficsit-companion/src/domain/$f.cpp     ficsit-companion/src/domain/snapshot/
done
```

- [ ] **Step 2: Rewrite include paths**

```bash
for h in factory_snapshot_model resource_flow; do
  grep -rl "domain/$h.hpp" ficsit-companion/src ficsit-companion/include ficsit-companion/tests \
    | xargs -r sed -i "s#domain/$h.hpp#domain/snapshot/$h.hpp#g"
done
```

- [ ] **Step 3: Update CMakeLists.txt**

In `HEADER_FILES`: `include/domain/factory_snapshot_model.hpp → include/domain/snapshot/factory_snapshot_model.hpp`, `include/domain/resource_flow.hpp → include/domain/snapshot/resource_flow.hpp`.
In `DOMAIN_SOURCE_FILES`: both `.cpp` → `src/domain/snapshot/<f>.cpp`.

- [ ] **Step 4: Build** — succeeds.
- [ ] **Step 5: Test** — baseline count, green.
- [ ] **Step 6: Mark progress** — check off Task 8.

---

### Task 9: Final sweep + documentation

**Files:**
- Verify: `ficsit-companion/src`, `include`, `tests`
- Modify: `C:\Users\adeni\.claude\projects\G--Projetos-satis-ficsit-companion-upgrade\memory\layered-architecture.md` and `MEMORY.md` pointer
- Modify: PROGRESS file

- [ ] **Step 1: Confirm no stale flat-domain includes remain**

Run:
```bash
grep -rnE 'domain/(building|factory_snapshot_model|fractional_number|game_data|graph_model|graph_item_resolve|json|linear_solve|link|node|node_data_resolver|node_display|pin|rate_solver|rate_solver_internal|recipe|resource_flow|vehicle_map|vehicle_map_camera|vehicle_map_query|vehicle_route)\.hpp' \
  ficsit-companion/src ficsit-companion/include ficsit-companion/tests
```
Expected: **only** matches that already carry a module segment (`domain/core/…`, `domain/nodes/…`, etc.). Any bare `domain/<file>.hpp` (no module) is a miss — fix it, rebuild, retest.

- [ ] **Step 2: Confirm the domain folders are empty of stray files**

Run:
```bash
ls ficsit-companion/include/domain ficsit-companion/src/domain
```
Expected: only the seven sub-folders (`core gamedata graph nodes snapshot solver vehicle`) under each — no loose `.hpp`/`.cpp`.

- [ ] **Step 3: Full clean rebuild + test**

Run:
```bash
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure
```
Expected: build succeeds; test count matches the Task 1 baseline; all green.

- [ ] **Step 4: Update the architecture memory**

In `layered-architecture.md`, add a paragraph recording the 2026-06-22 domain sub-module split (the 7 modules and their members) and that includes are now `domain/<module>/<file>.hpp`. Keep the existing layer-violation notes. Update the `MEMORY.md` one-line pointer hook if needed.

- [ ] **Step 5: Mark progress complete**

Check off Task 9 in the PROGRESS file and note the final passing test count.

---

## Self-Review

- **Spec coverage:** All 20 headers + 31 sources from the spec's mapping table are moved across Tasks 2–8 (core 2, gamedata 3, solver 6+1 private hdr, graph 4, nodes 10, vehicle 4, snapshot 2 sources; headers 2/3/2/4/3/4/2 = 20). The src-local `rate_solver_internal.hpp` is handled in Task 4. Non-goals (layer violations, infra/app structure) are honored — no task touches them. ✓
- **Placeholder scan:** No TBD/TODO; every rewrite gives the exact `sed` command and exact CMake string changes. ✓
- **Type consistency:** No symbols are introduced or renamed; only `#include` path strings change, each shown literally. The exact-`.hpp` matching rule is restated wherever prefix collisions are possible (`node`, `rate_solver`, `vehicle_map`). ✓
- **Verification:** Every module task ends with build + `ctest` against the recorded baseline count; Task 9 adds a grep sweep for any missed include. ✓
