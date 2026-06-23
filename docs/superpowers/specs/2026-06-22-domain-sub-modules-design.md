# Domain Layer Sub-Modules — Design

**Date:** 2026-06-22
**Status:** Approved (brainstorming) — pending spec review
**Type:** Structural refactor (pure move)

## Problem

The `domain/` layer has outgrown a flat folder. It holds **20 headers**
(`include/domain/`) and **31 sources** (`src/domain/`) in two flat directories,
while `infra/` (12+12) and `app/` (5+6) are appropriately sized. A 31-file
source folder is hard to scan and obscures the cohesive feature groups that
already exist inside the domain.

This refactor sub-structures `domain/` into seven cohesive modules. It is a
**pure move**: no logic changes, no public-API changes, and no change to the
`domain` / `infra` / `app` layer boundaries.

## Non-Goals

- **Not** fixing the 3 known cross-layer violations (`graph_model.cpp →
  infra/editor_backend.hpp`, `recipe.cpp` & `node.hpp → app/utils.hpp`,
  `infra/sav_import.cpp → app/utils.hpp`). Those remain exactly as-is; they are
  tracked separately and are out of scope for a sub-structuring move.
- **Not** touching `infra/` or `app/` folder structure.
- **Not** splitting or merging any translation unit. Every existing file moves
  intact.
- **Not** changing any test behavior.

## Approach

Seven feature modules, **full mirror**: matching sub-folders are created under
both `include/domain/<module>/` and `src/domain/<module>/`, keeping headers and
sources structurally parallel. Include paths become
`#include "domain/<module>/<file>.hpp"`, extending the existing
layer-qualified include convention one level deeper.

Rejected alternatives:
- *Coarser (4–5 modules)* — bigger buckets, less self-documenting, same churn.
- *src-only sub-folders* — leaves `include/domain/` a flat 20-file folder and
  breaks the src/include mirror. Rejected for inconsistency.

## File → Module Mapping

Every one of the 20 headers and 31 sources lands in exactly one module. The
src-local `rate_solver_internal.hpp` (a private header under `src/domain/`)
moves with the solver sources.

| Module | Headers (`include/domain/<module>/`) | Sources (`src/domain/<module>/`) |
|---|---|---|
| `core/` | fractional_number, json | fractional_number, json |
| `gamedata/` | building, game_data, recipe | building, game_data, recipe |
| `nodes/` | node, node_data_resolver, node_display | node_base, craft_node, group_node, organizer_nodes, sink_node, logistics_node, vehicle_station_node, extractor_node, node_data_resolver, node_display |
| `graph/` | pin, link, graph_model, graph_item_resolve | pin, link, graph_model, graph_item_resolve |
| `solver/` | rate_solver, linear_solve | rate_solver, rate_solver_apply, rate_solver_seed, rate_solver_system, rate_solver_variables, linear_solve, rate_solver_internal.hpp (src-local) |
| `vehicle/` | vehicle_map, vehicle_map_camera, vehicle_map_query, vehicle_route | vehicle_map, vehicle_map_camera, vehicle_map_query, vehicle_route |
| `snapshot/` | factory_snapshot_model, resource_flow | factory_snapshot_model, resource_flow |

Counts: headers 2+3+3+4+2+4+2 = 20 ✓. Sources 2+3+10+4+6+4+2 = 31 ✓ (plus the
src-local `rate_solver_internal.hpp`).

Module rationale:
- **core/** — foundational value/utility types with no game concept
  (exact-rational arithmetic, JSON helper).
- **gamedata/** — static game-content definitions parsed from
  `satisfactory.json` (`Item`/`Recipe`/`Building`/`GameData`).
- **nodes/** — the polymorphic `Node` family (`node.hpp` stays the unified
  header) plus per-node display and the data-resolver port used during node
  deserialization.
- **graph/** — graph topology and connectivity: `Pin`, `Link`, `GraphModel`,
  and item-resolution over the graph.
- **solver/** — rate propagation: the `rate_solver` phase set and the
  `linear_solve` Gaussian-elimination kernel.
- **vehicle/** — vehicle map (camera/query) and the `VehicleRoute` model.
- **snapshot/** — factory-snapshot model and the resource-flow report
  (analysis-output structures).

## Mechanics

1. **Move files.** Use `git mv` to relocate each header and source into its new
   module folder, preserving history.
2. **Rewrite include directives.** For each moved header, replace the exact
   string `domain/<file>.hpp` → `domain/<module>/<file>.hpp` across `src/`,
   `include/`, and `tests/`. Filenames are unique across the domain, so there
   are no ambiguous matches. The src-local include
   `"domain/rate_solver_internal.hpp"` → `"domain/solver/rate_solver_internal.hpp"`
   follows the same rule (6 call sites: the five `rate_solver*.cpp` and
   `tests/test_rate_solver_phases.cpp`). Documentation files under `docs/` are
   intentionally **not** rewritten.
3. **Update CMake** (`ficsit-companion/CMakeLists.txt`):
   - Repoint every path in `HEADER_FILES` (domain entries only) and
     `DOMAIN_SOURCE_FILES` to the new sub-folders.
   - The `source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src ...)` and
     `source_group(TREE .../include ...)` calls reflect the new tree
     automatically once the file lists are updated — no manual group edits.
   - `INFRA_SOURCE_FILES`, `APP_SOURCE_FILES`, and `TEST_SOURCE_FILES` are
     unchanged (test files do not move; only their `#include` lines change).

## Verification

- `cmake --build build --config Release --target fc-tests` compiles clean. The
  compiler is the safety net: any missed include rewrite fails the build with a
  clear "file not found".
- `ctest --test-dir build -C Release --output-on-failure` — the full Catch2
  suite stays green with zero behavior changes.
- Spot-check that no `#include "domain/<file>.hpp"` (un-prefixed by a module)
  remains in `src/`, `include/`, or `tests/` for any moved file.

## Risks

- **Missed include** → caught at compile time, not runtime. Low risk.
- **Merge-history noise** — mitigated by `git mv`.
- **Doc drift** — `docs/` references to old paths are left intact by design;
  they are historical plans/specs, not live code.

## Follow-Up (not in this work)

After the move, update the `layered-architecture` memory to record the new
domain sub-module taxonomy, and track progress in a `PROGRESS.md` per the
no-git-ops working preference.
