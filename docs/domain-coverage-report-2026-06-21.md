# Domain Layer Report — lines, test coverage

Date: 2026-06-21
Scope: `ficsit-companion/src/domain/*.cpp` and `ficsit-companion/include/domain/*.hpp`

Supersedes `domain-coverage-report-2026-06-18.md`. Reflects the rate-solver
refactor (Phase 1) and the domain coverage push (Phase 2, Tasks 7–13).

## Summary

- **Domain `.cpp` line coverage: 89.2%** (2,854 / 3,198 instrumented lines) —
  up from **72.2%** (2,300 / 3,185) on 2026-06-18.
- Including instrumented headers: **89.0%** (2,947 / 3,310).
- 27 `.cpp` files, ~5,467 lines.
- Collected from a single full-suite run: **196/196 tests, 1,180 assertions**.
- Full `fc-tests` Debug run no longer crashes under OpenCppCoverage (the
  2026-06-18 snapshot-builder segfault is resolved); coverage no longer needs
  the split-and-merge workaround.

## Per-file: `src/domain/*.cpp`

Sorted by coverage (lowest first). `lines` = raw file line count;
`coverage` = line coverage (OpenCppCoverage).

| file | lines | instrumented | covered | coverage |
|---|--:|--:|--:|--:|
| node_data_resolver.cpp | 15 | 9 | 4 | 44.4% |
| factory_snapshot_model.cpp | 21 | 15 | 8 | 53.3% |
| recipe.cpp | 109 | 72 | 47 | 65.3% |
| group_node.cpp | 486 | 322 | 238 | 73.9% |
| organizer_nodes.cpp | 221 | 150 | 120 | 80.0% |
| node_display.cpp | 117 | 61 | 50 | 82.0% |
| extractor_node.cpp | 205 | 111 | 92 | 82.9% |
| logistics_node.cpp | 123 | 69 | 59 | 85.5% |
| rate_solver.cpp | 948 | 465 | 404 | 86.9% |
| graph_model.cpp | 340 | 178 | 159 | 89.3% |
| fractional_number.cpp | 302 | 184 | 166 | 90.2% |
| sink_node.cpp | 63 | 36 | 33 | 91.7% |
| json.cpp | 867 | 499 | 460 | 92.2% |
| craft_node.cpp | 128 | 81 | 77 | 95.1% |
| node_base.cpp | 136 | 85 | 82 | 96.5% |
| vehicle_station_node.cpp | 60 | 32 | 31 | 96.9% |
| game_data.cpp | 104 | 53 | 52 | 98.1% |
| vehicle_map.cpp | 296 | 196 | 194 | 99.0% |
| graph_item_resolve.cpp | 183 | 132 | 131 | 99.2% |
| vehicle_route.cpp | 213 | 133 | 132 | 99.2% |
| vehicle_map_query.cpp | 101 | 66 | 66 | 100.0% |
| building.cpp | 16 | 8 | 8 | 100.0% |
| vehicle_map_camera.cpp | 71 | 52 | 52 | 100.0% |
| pin.cpp | 114 | 53 | 53 | 100.0% |
| link.cpp | 21 | 14 | 14 | 100.0% |
| resource_flow.cpp | 127 | 78 | 78 | 100.0% |
| linear_solve.cpp | 80 | 44 | 44 | 100.0% |
| **TOTAL** | **~5,467** | **3,198** | **2,854** | **89.2%** |

## Phase 2 movement (2026-06-18 → 2026-06-21)

| file | before | after |
|---|--:|--:|
| vehicle_map.cpp | 8.2% | 99.0% |
| graph_item_resolve.cpp | 35.6% | 99.2% |
| recipe.cpp | 45.8% | 65.3% (100% non-`Render`) |
| pin.cpp | 50.9% | 100.0% |
| graph_model.cpp | 62.4% | 89.3% |
| json.cpp | 66.3% | 92.2% |
| fractional_number.cpp | 67.9% | 90.2% |

Phase 1 left `rate_solver.cpp` at 86.9% (decomposed into 6 phase methods, no
coverage regression vs the 86.6% baseline).

## Remaining low-coverage code (improvement targets)

- `node_data_resolver.cpp` (44.4%) and `factory_snapshot_model.cpp` (53.3%) —
  tiny files; a few untested branches dominate the percentage.
- `recipe.cpp` (65.3%) — all 47 non-`Render` lines covered; the remainder is
  ImGui-dependent `Recipe::Render`.
- `group_node.cpp` (73.9%) — largest untested surface by absolute lines.

Most other gaps are ImGui `Render*`/`RenderInputText` paths that aren't unit
-testable without a UI harness.

## Known production defects / quirks surfaced by this push

- **Fixed (Task 12):** `json.cpp` parser preserved the backslash when decoding
  `\"`, corrupting any string containing a quote on each save/load round-trip.
  Fixed at the escaped-quote case to emit only the decoded quote; covered by a
  round-trip regression test.
- **Open (Task 13):** `FractionalNumber(const std::string&)` skips a leading
  decimal point — `".5"` parses to `5`, not `0.5` — because the scanner only
  begins a number token on a digit. The `decimal_index == 0` branch is therefore
  unreachable via the string constructor. Documented in tests, not yet fixed.

## Methodology

- **Lines**: raw file line counts.
- **Coverage**: `OpenCppCoverage` over a Debug `fc-tests` build, scoped to
  `src/domain` and `include/domain`. Output: `coverage-domain.xml` (cobertura)
  in repo root; per-file figures aggregated from the cobertura `<class>` entries.
