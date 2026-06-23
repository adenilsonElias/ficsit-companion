# Design — Rate Solver Phase I/O Decomposition

Date: 2026-06-22
Status: approved (pending written spec review)
Scope: `ficsit-companion/src/domain/rate_solver.cpp`

## Problem

`rate_solver.cpp` is ~1,000 lines in a single translation unit. The solver
logic lives in an anonymous-namespace `SolveContext` struct whose five phase
methods (`SeedRelevantPins`, `AssignVariables`, `BuildLinearSystem`,
`ReduceAndCheck`, `ApplyResults`) all read and mutate one shared mutable state
object. Consequences:

- The file is too large to hold in context comfortably; edits are riskier.
- Phases are only reachable through `RateSolver::Solve`, so they can only be
  tested end-to-end, never in isolation.
- The data flow between phases is implicit (every phase touches the whole
  context), so it is hard to see what each phase actually consumes and produces.

This is **not** a behavior change. The public API (`RateSolver::Solve`) and all
observable results stay identical; the 196-test suite is the safety net.

## Goal

Decompose the solver into free functions with **explicit inputs and outputs**,
split across focused translation units, and add **per-phase unit tests** that
exercise each function directly.

## Non-goals

- No algorithmic change. Phase bodies move near-verbatim.
- No attempt to make the two intrinsically side-effecting phases pure
  (`SeedRelevantPins` sets `link->flow`; `ApplyResults` mutates pins/nodes/links).
  Eliminating those side effects would be a much larger change and is out of
  scope.
- No change to `RateSolver::Solve`'s signature or to `rate_solver.hpp`.

## Approach

Chosen: **free functions + per-phase result structs** in a named namespace
`rate_solver_detail`, declared in a new internal header and defined one phase per
`.cpp`. The named namespace (vs the current anonymous one) gives the functions
external linkage so they can live in separate translation units and be called
from tests.

Rejected alternatives:
- *Keep the fat `SolveContext`, split files only* — testing a phase would still
  require constructing the whole context; the testability goal is not met.
- *Phase objects (a class per phase)* — boilerplate with no polymorphism payoff
  over plain functions.

## Data-flow structs

Declared in `src/domain/rate_solver_internal.hpp` (implementation-detail header,
not under `include/`):

```cpp
namespace rate_solver_detail
{
    struct ActiveGroup { std::vector<Pin*> supply; std::vector<Pin*> demand; };

    // Shared inputs threaded into every phase that needs them.
    struct SolveInputs
    {
        std::vector<std::unique_ptr<Node>>& nodes;
        std::vector<std::unique_ptr<Link>>& links;
        const Pin* constraint_pin;
        const FractionalNumber& constraint_value;
    };

    struct SeedResult
    {
        std::unordered_set<const Pin*> relevant_pins;
        std::unordered_set<const Pin*> multi_pin_constrained;
        std::vector<ActiveGroup> active_groups;
    };

    struct VariableMapping
    {
        std::unordered_map<const Pin*, std::pair<size_t, FractionalNumber>> associated_variable_index;
        size_t num_variables = 0;
        std::vector<size_t> group_total_index;
        std::unordered_map<size_t, FractionalNumber> group_total_default;
        std::unordered_map<const Pin*, std::pair<size_t, bool>> group_membership;
        std::vector<const Pin*> reversed_variable_map;
    };

    struct LinearSystem
    {
        std::vector<std::vector<FractionalNumber>> equations_coefficients;
        std::vector<FractionalNumber> constants;
    };

    struct ReducedSystem
    {
        std::vector<std::vector<FractionalNumber>> reduced_matrix;
    };
}
```

## Phase functions

Declared in the internal header, defined one per `.cpp`:

```cpp
SeedResult      SeedRelevantPins(const SolveInputs& in);
VariableMapping AssignVariables(const SeedResult& seed);
LinearSystem    BuildLinearSystem(const SolveInputs& in,
                                  const SeedResult& seed,
                                  const VariableMapping& vars);
std::optional<ReducedSystem> ReduceAndCheck(const LinearSystem& sys,
                                            const VariableMapping& vars,
                                            float& error_time,
                                            float error_flow_duration);
bool ApplyResults(const SolveInputs& in,
                  const SeedResult& seed,
                  const VariableMapping& vars,
                  LinearSystem& sys,          // ApplyResults appends equations + re-reduces
                  ReducedSystem& reduced,
                  float& error_time,
                  float error_flow_duration);
```

Notes:
- `ReduceAndCheck` returns `std::nullopt` on the failure paths where it currently
  returns `false` (and sets `error_time` on those paths, preserving semantics).
- `ApplyResults` takes `LinearSystem&` and `ReducedSystem&` non-const because its
  free-variable resolution loop appends equations and re-runs `ReduceMatrix`,
  exactly as today.
- `error_time` stays a by-reference out-param to preserve the exact error-flash
  semantics the UI depends on.

## File layout

| file | contents |
|---|---|
| `src/domain/rate_solver_internal.hpp` | structs + phase function declarations |
| `src/domain/rate_solver.cpp` | `RateSolver::Solve` orchestrator + `ResetGraphState` (file-local) |
| `src/domain/rate_solver_seed.cpp` | `SeedRelevantPins` + `BeltFarEnd` (file-local) + `DEBUG_PROPAGATION` statics |
| `src/domain/rate_solver_variables.cpp` | `AssignVariables` |
| `src/domain/rate_solver_system.cpp` | `BuildLinearSystem` + `ReduceAndCheck` |
| `src/domain/rate_solver_apply.cpp` | `ApplyResults` |

The internal header includes `fractional_number.hpp` (used by value in structs)
and forward-declares `Node`, `Link`, `Pin`, `Item`; each `.cpp` includes the
heavy headers (`node.hpp`, `pin.hpp`, `link.hpp`, etc.) it needs.

Orchestrator after decomposition:
```cpp
ResetGraphState(nodes, links, error_time);
rate_solver_detail::SolveInputs in{ nodes, links, constraint_pin, constraint_value };
const auto seed = SeedRelevantPins(in);
const auto vars = AssignVariables(seed);
auto sys = BuildLinearSystem(in, seed, vars);
auto reduced = ReduceAndCheck(sys, vars, error_time, error_flow_duration);
if (!reduced) return false;
return ApplyResults(in, seed, vars, sys, *reduced, error_time, error_flow_duration);
```

## Migration mechanics (risk)

Phase bodies move near-verbatim, but every bare member access of the old
`SolveContext` is rewritten to qualified struct-field access, e.g.
`relevant_pins` → `seed.relevant_pins`, `num_variables` → `vars.num_variables`,
`associated_variable_index` → `vars.associated_variable_index`,
`equations_coefficients` → `sys.equations_coefficients`,
`reduced_matrix` → `reduced.reduced_matrix`. This mechanical rewrite is the main
source of risk, so phases are migrated **one at a time with the full 196-test
suite run green after each step**.

## Testing

- Existing `tests/test_rate_solver.cpp` is unchanged and remains the end-to-end
  safety net.
- New `tests/test_rate_solver_phases.cpp` adds focused unit tests per phase,
  driven through the internal header:
  - `SeedRelevantPins`: relevant-pin set, multi-pin-constrained set, and active
    route groups for representative small graphs.
  - `AssignVariables`: variable count, per-pin ratios (craft base rate,
    somersloop multiplier, extractor purity, game-splitter share), group-total
    variable allocation, reversed map.
  - `BuildLinearSystem`: user-constraint equation, per-link equality, merger/
    splitter balance, route-group balance.
  - `ReduceAndCheck`: success, over-determined all-zero-rows, inconsistent row
    (returns `nullopt`, sets `error_time`).
  - `ApplyResults`: free-variable resolution, negative-rate rejection, link-flow
    clearing, final pin/node rate assignment.
- Register the new test file in `ficsit-companion/CMakeLists.txt`
  (`TEST_SOURCE_FILES`). Register the new `.cpp` files in `DOMAIN_SOURCE_FILES`.

## Acceptance criteria

- Full suite green (≥196 tests, plus the new per-phase cases).
- `rate_solver*.cpp` line coverage ≥ the current 86.9% (no regression).
- No file in the set exceeds ~370 lines (`ApplyResults`); the rest < ~210.
- `RateSolver::Solve` behavior bit-for-bit identical (verified by the unchanged
  end-to-end suite).

## Process notes

- No git commits (standing user preference). Progress tracked in the existing
  Phase-2 PROGRESS ledger or a new one for this work.
