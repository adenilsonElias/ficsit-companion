# Rate Solver Refactor + Domain Coverage — Design

Date: 2026-06-20
Status: Approved (brainstorming)
Scope: `ficsit-companion/src/domain/rate_solver.cpp` refactor; test coverage for
sub-70% domain `.cpp` files.

## Problem

Two issues surfaced by `docs/domain-coverage-report-2026-06-18.md`:

1. **`rate_solver.cpp` is a god function.** `RateSolver::Solve` spans lines
   70–957 — a single ~887-line function carrying many responsibilities
   (pin seeding, variable assignment, equation building, solving, result
   application). The file is well-tested (86.6%) but unmaintainable: the
   problem is SRP/maintainability, not coverage.
2. **Several domain files have low coverage.** Seven `.cpp` files sit below
   70% line coverage, dragging overall domain coverage to 72.2%.

## Goals

- Decompose `RateSolver::Solve` into named, independently understandable steps
  with **no behavior change**.
- Raise every sub-70% domain `.cpp` toward ~80% line coverage; lift overall
  domain line coverage above its current 72.2%.

## Non-Goals (YAGNI)

- Changing the linear-solver algorithm or numeric behavior.
- Touching the app/infra layers or the 3 known cross-layer violations.
- Adding tests for files already ≥70%.
- Reaching dead/unreachable code by contorting tests — flag it instead.

---

## Workstream 1 — `rate_solver` decomposition

### Approach

Introduce a private `SolveContext` in `rate_solver.cpp`'s **anonymous
namespace** (not exposed in `rate_solver.hpp`). The public API stays exactly:

```cpp
static bool RateSolver::Solve(nodes, links, constraint_pin,
                              constraint_value, error_time, error_flow_duration);
```

`SolveContext` holds the shared working state currently living as locals in
`Solve` (the propagation queues, `relevant_pins`, `multi_pin_constrained`,
`associated_variable_index`, `reversed_variable_map`, active route groups, the
equation matrix `equations_coefficients`/`constants`, etc.). `Solve` becomes a
thin orchestrator constructing the context and calling each step in order.

### Phase → method mapping

| Phase | Method | Current lines (approx) |
|---|---|---|
| 0. Reset pin errors / link flow | `ResetGraphState` *(already extracted)* | 46–67 |
| 1. Seed & collect relevant pins — per node-type propagation, multi-pin "strong/weak" constraints, vehicle-route group expansion | `SeedRelevantPins()` | 85–303 |
| 2. Assign linear-system variables — per node-type ratios (craft/group/splitter/extractor/merger/sink), auxiliary route "total" variables, reverse & membership maps | `AssignVariables()` | 305–443 |
| 3. Build equations — user constraint, per-link equality, merger/splitter balance, route-group balance | `BuildLinearSystem()` | 445–550 |
| 4. Gaussian reduce + consistency checks | `ReduceAndCheck()` | 551–593 |
| 5. Resolve free variables, write `current_rate` back to pins, set link flow, error handling, return value | `ApplyResults()` | 594–957 |

Phase 1 is the largest. If it remains unwieldy after extraction, split the
vehicle-route group expansion into a `CollectRouteGroups()` sub-step.

The `create_variable` lambda in phase 2 becomes a `SolveContext` method or a
helper called from `AssignVariables()`.

### Constraints

- **Strictly behavior-preserving.** No change to numeric results, error paths,
  return values, or `error_time` semantics.
- The existing `tests/test_rate_solver.cpp` (314 lines) is the safety net: it
  must stay green before and after every extraction.
- `DEBUG_PROPAGATION` debug scaffolding is preserved as-is.

### Verification

- `fc-tests` Release suite green (currently 136/136) before and after.
- `rate_solver.cpp` line coverage stays ≥86.6% (no logic silently dropped).

---

## Workstream 2 — Domain coverage push

### Targets (sub-70%, lowest first)

| file | now | notes |
|---|--:|---|
| `vehicle_map.cpp` | 8.2% | JSON import/parse logic; `test_vehicle_map_import.cpp` exists but barely exercises it — biggest single win; needs representative JSON fixtures |
| `graph_item_resolve.cpp` | 35.6% | item/recipe resolution |
| `recipe.cpp` | 45.8% | recipe data + helpers |
| `pin.cpp` | 50.9% | pin rate/lock logic |
| `graph_model.cpp` | 62.4% | graph model |
| `json.cpp` | 66.3% | parser/serializer |
| `fractional_number.cpp` | 67.9% | rational arithmetic + expression parsing |

Target: each toward **~80%** line coverage; overall domain line coverage above
72.2%.

### Approach per file

1. Read the file; run OpenCppCoverage to identify cold lines/branches.
2. Add Catch2 cases under `ficsit-companion/tests/`, following existing
   patterns. Extend the existing `test_<file>.cpp` where one exists; otherwise
   add a new one.
3. Pure-logic files (`fractional_number`, `recipe`, `pin`, `json`,
   `graph_item_resolve`, `graph_model`) → direct unit tests.
   `vehicle_map.cpp` → JSON-fixture-driven import tests.
4. Re-run coverage; record before/after %.

### Constraints

- **Tests only — no production behavior changes** in this workstream.
- Genuinely dead/unreachable code is flagged, not chased.

---

## Sequencing

1. **Phase 1: rate_solver refactor** — isolated, protected by existing tests;
   clean the code before adding a wave of new tests.
2. **Phase 2: coverage push** — independent files, in the order listed above.

## Verification & measurement

- Build/test per `CLAUDE.md`. Coverage via a single Debug `fc-tests` run +
  OpenCppCoverage (the prior Debug full-suite segfault is resolved, so the
  split-run workaround is no longer needed).
- After each coverage file: re-run and record before/after.
- Final deliverable: a refreshed `docs/domain-coverage-report-2026-06-20.md`.

## Process

- Progress tracked in a `*-PROGRESS.md` alongside the implementation plan.
- **No git commits/branches** unless explicitly requested (standing user
  preference). Spec and plan files are written but not committed.
