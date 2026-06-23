# Progress — Rate Solver Phase I/O Decomposition

Plan: `2026-06-22-rate-solver-phase-decomposition.md`
Spec: `../specs/2026-06-22-rate-solver-phase-decomposition-design.md`
Execution: subagent-driven, **no git commits** (working-tree only, per user preference).
Per-task diffs isolated via file snapshots in `tmp/sdd-decomp/` (gitignored); review packages saved as `task-decomp-N-review-package.diff`.

## Baseline
- 2026-06-22: Full suite green **196/196** (Release). `rate_solver.cpp` single-file, ~1005 lines, coverage 86.9% (404/465) from the 2026-06-21 report.

## Tasks
- [x] Task 1: Internal header with structs + declarations (+ include-path wiring)
- [x] Task 2: Recompose SolveContext from the structs (qualify all accesses)
- [x] Task 3: Extract SeedRelevantPins → rate_solver_seed.cpp
- [x] Task 4: Extract AssignVariables → rate_solver_variables.cpp
- [x] Task 5: Extract BuildLinearSystem + ReduceAndCheck → rate_solver_system.cpp
- [x] Task 6: Extract ApplyResults → rate_solver_apply.cpp + delete SolveContext
- [x] Task 7: Per-phase unit tests

## Minor findings (for final review triage)
- Task 3: `rate_solver_seed.cpp` relies on transitive `<unordered_set>` (used only in DEBUG_PROPAGATION dead code) — add explicit include for hygiene.
- Task 3: `rate_solver.cpp` retains now-dead includes — RESOLVED in Task 6 (14 dead includes trimmed).
- Task 6: `rate_solver.cpp` `Solve` keeps a `#if DEBUG_PROPAGATION fprintf` block but `<cstdio>` was trimmed — safe at macro=0, would break compile if flipped to 1. Same class as the seed.cpp `<unordered_set>` Minor. Cheap fix: re-add `<cstdio>` (and `<unordered_set>` in seed.cpp) or guard with a comment.
- Task 7: test 3 (`BuildLinearSystem`) asserts `equations_coefficients.size() >= 2` (loose; exact is 2). test 4 (`ApplyResults`) only checks `ins[0]` of the even split (ins[1]/ins[2] covered by the integration test in test_rate_solver.cpp). Minor test-tightness.

## Log
- 2026-06-22: Ledger created; baseline 196/196.
- 2026-06-22: Task 1 complete — `rate_solver_internal.hpp` created (6 structs + 5 phase decls), `src` added to fc-core/fc-tests include paths, `ActiveGroup` moved to header. Suite 196/196. Review clean (spec ✅, quality approved).
- 2026-06-22: Task 2 complete — SolveContext recomposed into 5 structs (in/seed/vars/sys/reduced) + error fields; all 5 method bodies qualified via substitution table; `solution` became a local in ApplyResults (verified equivalent). Suite 196/196. Review clean — reviewer verified all equation signs/loop bounds vs baseline.
- 2026-06-22: Task 3 complete — `SeedRelevantPins` extracted to `rate_solver_seed.cpp` (free fn + BeltFarEnd + debug statics moved); orchestrator calls `ctx.seed = SeedRelevantPins(ctx.in)`; CMake registered. Suite 196/196. Review clean (spec ✅, quality approved, 2 Minor logged).
- 2026-06-22: Task 4 complete — `AssignVariables` extracted to `rate_solver_variables.cpp` (verbatim body, all per-kind ratios verified); orchestrator calls `ctx.vars = AssignVariables(ctx.seed)`; CMake registered. Suite 196/196. Review clean (spec ✅, quality approved, no findings).
- 2026-06-22: Task 5 complete — `BuildLinearSystem` + `ReduceAndCheck` extracted to `rate_solver_system.cpp`; ReduceAndCheck now returns `std::optional<ReducedSystem>` (2 nullopt paths keep error_time assignment, success returns reduced); orchestrator threads optional into `ctx.reduced`; CMake registered. Suite 196/196. Review clean (spec ✅, quality approved, no findings).
- 2026-06-22: Task 6 complete — `ApplyResults` extracted to `rate_solver_apply.cpp` (388 lines, verbatim body); `SolveContext` fully deleted; `Solve` rewritten to thread locals; 14 dead includes trimmed from rate_solver.cpp (now 60 lines); CMake registered. Suite 196/196. Review clean (spec ✅, quality approved, 1 Minor logged: cstdio/DEBUG). File sizes: rate_solver.cpp 60, apply 388, seed 243, system 170, variables 153.
- 2026-06-22: Task 7 complete — `test_rate_solver_phases.cpp` added (5 cases, 15 assertions): one per phase + a ReduceAndCheck rejection case (locked inputs vs driven output → nullopt + error_time set). Caught + corrected a wrong anchor assertion from the brief (merger output is not multi_pin_constrained). Full suite **201/201**. Review clean (spec ✅, quality approved, 2 Minor logged).
- 2026-06-22: **Coverage gate PASSED** — full-suite `rate_solver*.cpp` combined **88.8% (419/472)** vs 86.9% baseline (+1.9pp, no regression). Per-file: rate_solver.cpp 100%, system 98.9%, seed 92.6%, variables 90.5%, apply 80.7%. All 7 tasks complete; proceeding to final whole-branch review.
- 2026-06-22: **Final whole-branch review (opus): READY TO COMPLETE.** No Critical/Important. Confirmed `SolveContext` fully removed (zero refs), public API byte-identical, lifetimes/const-ref passing sound, struct cut-points well-chosen. Triage: Minor B (seed.cpp unordered_set) declared a non-issue (internal.hpp includes it directly); Minors C/D deferred (covered elsewhere / soft cap on verbatim body); two pre-existing debug-only nits noted (not introduced here). Applied the one endorsed zero-risk fix: re-added `<cstdio>` to rate_solver.cpp. Full suite **201/201**. **PLAN COMPLETE.**
