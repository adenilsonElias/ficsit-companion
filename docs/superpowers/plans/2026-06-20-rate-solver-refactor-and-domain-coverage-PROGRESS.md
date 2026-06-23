# Progress — Rate Solver Refactor + Domain Coverage

Plan: `2026-06-20-rate-solver-refactor-and-domain-coverage.md`
Execution: subagent-driven, **no git commits** (working-tree only, per user preference).

## Baseline
- 2026-06-20: Release `fc-tests` green — **137/137** passing (~10s). `rate_solver.cpp` clean in git. Toolchain (cmake 4.3.3 / ctest / MSVC) verified.

## Phase 1 — rate_solver decomposition
- [x] Task 1: Introduce SolveContext, route state through ctx
- [x] Task 2: Extract SeedRelevantPins()
- [x] Task 3: Extract AssignVariables()
- [x] Task 4: Extract BuildLinearSystem()
- [x] Task 5: Extract ReduceAndCheck()
- [x] Task 6: Extract ApplyResults(), finalize orchestrator

## Phase 2 — domain coverage push
- [x] Task 7: vehicle_map.cpp (8.2% → 98.5%)
- [x] Task 8: graph_item_resolve.cpp (35.6% → 99.2%)
- [x] Task 9: recipe.cpp (45.8% → 65.3%; 100% non-Render)
- [x] Task 10: pin.cpp (50.9% → 100%)
- [x] Task 11: graph_model.cpp (62.4% → 89.3%)
- [x] Task 12: json.cpp (66.3% → 90.6%) + fixed escaped-quote round-trip defect
- [x] Task 13: fractional_number.cpp (67.9% → 90.2%)
- [x] Task 14: Refresh coverage report (domain .cpp 72.2% → 89.2%)

## Resume notes (for Phase 2)
- **ALL TASKS COMPLETE 2026-06-21.** Phases 1 (Tasks 1–6) and 2 (Tasks 7–14) done. Full suite green **196/196 (1,180 assertions)**. Domain `.cpp` coverage **72.2% → 89.2%**. Refreshed report at `docs/domain-coverage-report-2026-06-21.md` (supersedes the 2026-06-18 one). One production defect fixed (json escaped-quote, Task 12, user-authorized); one quirk documented but unfixed (fractional leading-dot parse, Task 13 — needs authorization if it should be fixed). No subagents in flight. No git commits (per user preference).
- New test files must be added explicitly to `ficsit-companion/CMakeLists.txt` in the `TEST_SOURCE_FILES` list (lines ~191–214) — there is no glob. Existing test files for the Phase 2 targets: `test_vehicle_map_import.cpp`, `test_graph_model.cpp`, `test_rate_solver.cpp`, `test_node_display.cpp` exist; `test_recipe.cpp`, `test_pin.cpp`, `test_json.cpp`, `test_fractional_number.cpp`, `test_graph_item_resolve.cpp` do NOT yet exist (must be created + registered).
- Coverage cmd (PowerShell): build Debug fc-tests, then `& "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe" --quiet --sources ...\src\domain --sources ...\include\domain --excluded_sources ...\build --export_type cobertura:coverage-domain.xml -- ...\build\ficsit-companion\Debug\fc-tests.exe`. Parse with `[xml]` + filter classes by filename.
- Snapshots of rate_solver.cpp after each task are in `tmp/sdd/` (gitignored).

## Minor findings (for final review triage)
- Task 3: comment Unicode drift in rate_solver.cpp (~line 379) — `× purity` became `x purity`. Cosmetic, no behavior impact.
- Task 5: ReduceAndCheck() defined out of pipeline order (between AssignVariables and BuildLinearSystem). Cosmetic readability only; no functional impact.

## Log
- 2026-06-20: Baseline established, ledger created.
- 2026-06-20: Task 1 done — SolveContext introduced, state routed through ctx, suite green (137/137). Review clean (spec ✅, quality approved; only trivial non-issues noted).
- 2026-06-20: Task 2 done — SeedRelevantPins() extracted into anonymous namespace method, ctx.SeedRelevantPins() call replaces region in Solve, suite green (137/137). Review clean (spec ✅, quality approved).
- 2026-06-20: Task 3 done — AssignVariables() extracted into anonymous namespace method (create_variable lambda + two pin passes + group_total/group_membership/reversed_variable_map construction), ctx.AssignVariables() call replaces region in Solve, suite green (137/137). rate_solver.hpp unchanged, no git commit.
- 2026-06-20: Task 4 done — BuildLinearSystem() extracted into anonymous namespace method (user-constraint equation, per-link equality, merger/splitter balance, per-route-group balance), ctx.BuildLinearSystem() call replaces region in Solve, suite green (137/137). rate_solver.hpp unchanged, no git commit. Review clean (spec ✅, quality approved, no findings).
- 2026-06-20: Task 5 done — ReduceAndCheck() extracted into anonymous namespace method (ReduceMatrix call + over-determined all-zero-rows check + inconsistent-row check), if (!ctx.ReduceAndCheck()) return false; replaces region in Solve, suite green (137/137). rate_solver.hpp unchanged, no git commit. Review clean (spec ✅, quality approved).
- 2026-06-20: Task 6 done — ApplyResults() extracted; Solve() now a 10-line orchestrator over the 6 phase methods. Suite green (137/137). Review clean (spec ✅, quality approved). **Coverage gate: rate_solver.cpp 86.9% (≥86.6% baseline — no regression).** Overall domain 72.5%. Debug full-suite runs clean under OpenCppCoverage (segfault confirmed resolved). **PHASE 1 COMPLETE.**
- 2026-06-20: Task 7 done — fixture-driven vehicle-map parser tests added; focused suite green (27 cases, 186 assertions), full suite green (143/143), `vehicle_map.cpp` coverage **8.2% → 98.5% (193/196)**. Spec and quality reviews approved; no production changes.
- 2026-06-20: Task 8 done — graph item resolution tests added and registered; focused suite green (11 cases, 61 assertions), full suite green (154/154), `graph_item_resolve.cpp` coverage **35.6% → 99.2% (131/132)**. Spec and quality reviews approved; no production changes.
- 2026-06-20: Task 9 done — recipe tests added and registered; focused suite green (6 cases, 40 assertions), full suite green (160/160), `recipe.cpp` coverage **45.8% → 65.3% (47/72)**. All **47/47 non-`Render` lines are covered**; the remaining 25 lines are ImGui-dependent `Recipe::Render`. Spec and quality reviews approved.
- 2026-06-20: Task 10 done — pin tests added and registered; focused suite green (7 cases, 88 assertions), full suite green (167/167), `pin.cpp` coverage **50.9% → 100% (53/53)**. Spec and quality reviews approved.
- 2026-06-20: Task 11 done — graph-model tests extended; focused suite green (19 cases, 100 assertions), full suite green (177/177), `graph_model.cpp` coverage **62.4% → 89.3% (159/178)**. Spec review approved; quality review found no blocking issues.
- 2026-06-21: Task 12 done — json tests added/registered + **production defect fixed** (user-authorized). TDD: added escaped-quote round-trip regression test (RED: `\"` decoded to `\"`), fixed `json.cpp:714` to emit only the decoded quote (GREEN). Focused suite green (11 cases, 138 assertions), full suite green (188/188), `json.cpp` coverage **66.3% → 90.6% (452/499)**. Spec + quality review passed inline.
- 2026-06-21: Task 13 done — `test_fractional_number.cpp` added/registered (8 cases, 46 assertions): numeric ctor sign/zero normalization, string parsing (ints/decimals/expressions/precedence/parens/whitespace), malformed-expression throws, lazy string getters, compound + free operators, comparisons. Full suite green (196/196), `fractional_number.cpp` coverage **67.9% → 90.2% (166/184)**. Remaining uncovered = `RenderInputText` (ImGui) + unreachable `decimal_index==0` branch. **Quirk documented (not asserted as correct): leading `.5` parses to 5, not 0.5** — scanner only starts a number on a digit, so the leading `.` is skipped; same reason the `decimal_index==0` branch is dead via the string ctor. Candidate for a future fix. No production changes.
- 2026-06-21: Task 14 done — regenerated full domain coverage from a single clean full-suite Debug run (196/196, 1,180 assertions; no segfault, no split/merge workaround needed). Wrote `docs/domain-coverage-report-2026-06-21.md` superseding the 2026-06-18 report. **Domain `.cpp` coverage 72.2% → 89.2% (2,854/3,198).** **PHASE 2 COMPLETE — plan finished.**
