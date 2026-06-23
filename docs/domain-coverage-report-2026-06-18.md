# Domain Layer Report — lines, functions, test coverage

Date: 2026-06-18
Scope: `ficsit-companion/src/domain/*.cpp` and `ficsit-companion/include/domain/*.hpp`

## Summary

- **Domain `.cpp` line coverage: 72.2%** (2,300 / 3,185 instrumented lines).
- Including instrumented headers: **72.4%** (2,374 / 3,278).
- 27 `.cpp` files, ~6,014 lines, ~235 function definitions.
- `include/domain`: 20 headers, 1,546 lines (mostly declarations).

## Per-file: `src/domain/*.cpp`

Sorted by coverage (lowest first). `funcs` = function/method definitions
(approximate, see Methodology); `coverage` = line coverage (OpenCppCoverage).

| file | lines | funcs | instrumented | covered | coverage |
|---|--:|--:|--:|--:|--:|
| vehicle_map.cpp | 321 | 10 | 196 | 16 | 8.2% |
| graph_item_resolve.cpp | 198 | 7 | 132 | 47 | 35.6% |
| node_data_resolver.cpp | 20 | 2 | 9 | 4 | 44.4% |
| recipe.cpp | 127 | 6 | 72 | 33 | 45.8% |
| pin.cpp | 124 | 4 | 53 | 27 | 50.9% |
| factory_snapshot_model.cpp | 25 | 2 | 15 | 8 | 53.3% |
| graph_model.cpp | 353 | 6 | 178 | 111 | 62.4% |
| json.cpp | 993 | 41 | 499 | 331 | 66.3% |
| fractional_number.cpp | 337 | 10 | 184 | 125 | 67.9% |
| group_node.cpp | 529 | 12 | 322 | 229 | 71.1% |
| organizer_nodes.cpp | 266 | 24 | 150 | 113 | 75.3% |
| extractor_node.cpp | 233 | 18 | 111 | 87 | 78.4% |
| node_display.cpp | 123 | 4 | 61 | 50 | 82.0% |
| logistics_node.cpp | 139 | 7 | 69 | 59 | 85.5% |
| rate_solver.cpp | 958 | 4 | 456 | 395 | 86.6% |
| sink_node.cpp | 77 | 6 | 36 | 33 | 91.7% |
| craft_node.cpp | 148 | 10 | 81 | 77 | 95.1% |
| node_base.cpp | 167 | 20 | 85 | 82 | 96.5% |
| vehicle_station_node.cpp | 70 | 7 | 32 | 31 | 96.9% |
| game_data.cpp | 113 | 5 | 49 | 48 | 98.0% |
| vehicle_route.cpp | 223 | 7 | 133 | 132 | 99.2% |
| building.cpp | 19 | 1 | 8 | 8 | 100.0% |
| linear_solve.cpp | 89 | 1 | 44 | 44 | 100.0% |
| link.cpp | 28 | 2 | 14 | 14 | 100.0% |
| resource_flow.cpp | 142 | 5 | 78 | 78 | 100.0% |
| vehicle_map_camera.cpp | 81 | 8 | 52 | 52 | 100.0% |
| vehicle_map_query.cpp | 111 | 6 | 66 | 66 | 100.0% |
| **TOTAL** | **~6,014** | **235** | **3,185** | **2,300** | **72.2%** |

## Per-file: `include/domain/*.hpp` (line counts)

| file | lines | file | lines |
|---|--:|---|--:|
| json.hpp | 464 | resource_flow.hpp | 48 |
| node.hpp | 331 | recipe.hpp | 62 |
| vehicle_map.hpp | 116 | vehicle_route.hpp | 66 |
| recipe.hpp | 62 | vehicle_map_camera.hpp | 58 |
| node_display.hpp | 34 | fractional_number.hpp | 49 |
| graph_model.hpp | 32 | vehicle_map_query.hpp | 43 |
| factory_snapshot_model.hpp | 33 | pin.hpp | 32 |
| game_data.hpp | 29 | node_data_resolver.hpp | 29 |
| building.hpp | 20 | link.hpp | 27 |
| graph_item_resolve.hpp | 22 | linear_solve.hpp | 20 |
| rate_solver.hpp | 31 | | |

Total: 20 headers, 1,546 lines.

## Lowest-covered domain code (improvement targets)

- `vehicle_map.cpp` — 8.2%
- `graph_item_resolve.cpp` — 35.6%
- `recipe.cpp` — 45.8%, `pin.cpp` — 50.9%
- `graph_model.cpp` — 62.4%, `json.cpp` — 66.3%, `fractional_number.cpp` — 67.9%

## Methodology

- **Lines**: raw file line counts.
- **Functions** ("flow"): comment/string-stripped regex counting `name(...) {`
  definitions, excluding control-flow keywords. Approximate (±few) — operator
  overloads, macro-generated, or multi-paren signatures may be miscounted.
- **Coverage**: `OpenCppCoverage` over a Debug `fc-tests` build, scoped to the
  domain source/include dirs. Output: `coverage-domain.xml` (cobertura) in repo
  root.

## Finding: Debug-only full-suite crash

The full `fc-tests` suite **segfaults in the Debug build** (around
`tests/test_factory_snapshot_builder.cpp:83`) — but only when the
`[snapshot_builder]` tests run together with the rest of the suite. Run in
isolation, `[snapshot_builder]` (5 cases) and `~[snapshot_builder]` (131 cases)
both pass, and the **Release** suite passes **136/136**. This points to a
test-interaction / global-state issue (likely game-data load order), not a
production-code regression.

Coverage above was collected by running the two groups separately
(binary export) and merging, to avoid the crash truncating the run.
