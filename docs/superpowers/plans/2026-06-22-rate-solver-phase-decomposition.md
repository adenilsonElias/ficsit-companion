# Rate Solver Phase I/O Decomposition — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `rate_solver.cpp` (~1,000 lines, one anonymous-namespace `SolveContext`) into a thin orchestrator plus five explicit phase functions with typed inputs/outputs across focused files, and add per-phase unit tests — with zero behavior change.

**Architecture:** Phases become free functions in namespace `rate_solver_detail`, declared in `src/domain/rate_solver_internal.hpp`, defined one phase per `.cpp`. Data flows through explicit structs (`SolveInputs`, `SeedResult`, `VariableMapping`, `LinearSystem`, `ReducedSystem`). `RateSolver::Solve` threads them. Spec: `docs/superpowers/specs/2026-06-22-rate-solver-phase-decomposition-design.md`.

**Tech Stack:** C++17, CMake, Catch2, OpenCppCoverage (MSVC).

## Global Constraints

- **No git commits** (standing user preference). Each task ends by running the full suite and updating the progress ledger, not committing.
- **No behavior change.** `RateSolver::Solve` signature and `rate_solver.hpp` are untouched. Method bodies move verbatim except for the identifier substitutions in the table below.
- **The full Catch2 suite (currently 196 tests) must pass after every task.** It is the safety net for this refactor.
- Build (Release): `cmake --build build --config Release --target fc-tests`
- Run suite (Release): `ctest --test-dir build -C Release --output-on-failure`
- New `.cpp` files go in `DOMAIN_SOURCE_FILES`; new test files go in `TEST_SOURCE_FILES` (no globbing). After adding any file, re-run `cmake -S . -B build` before building.

## Identifier substitution table (used in Tasks 2–6)

When moving method bodies, every bare `SolveContext` member reference is rewritten to qualified struct-field access. Bodies are otherwise unchanged.

| old (SolveContext member) | new (qualified) |
|---|---|
| `nodes` | `in.nodes` |
| `links` | `in.links` |
| `constraint_pin` | `in.constraint_pin` |
| `constraint_value` | `in.constraint_value` |
| `relevant_pins` | `seed.relevant_pins` |
| `multi_pin_constrained` | `seed.multi_pin_constrained` |
| `active_groups` | `seed.active_groups` |
| `associated_variable_index` | `vars.associated_variable_index` |
| `num_variables` | `vars.num_variables` |
| `group_total_index` | `vars.group_total_index` |
| `group_total_default` | `vars.group_total_default` |
| `group_membership` | `vars.group_membership` |
| `reversed_variable_map` | `vars.reversed_variable_map` |
| `equations_coefficients` | `sys.equations_coefficients` |
| `constants` | `sys.constants` |
| `reduced_matrix` | `reduced.reduced_matrix` |
| `error_time` | `error_time` (unchanged; member in Tasks 2–4, by-ref param in Tasks 5–6) |
| `error_flow_duration` | `error_flow_duration` (same) |

---

### Task 1: Internal header with structs + declarations

**Files:**
- Create: `ficsit-companion/src/domain/rate_solver_internal.hpp`
- Modify: `ficsit-companion/src/domain/rate_solver.cpp` (include the header; remove local `ActiveGroup`)
- Modify: `ficsit-companion/CMakeLists.txt` (add `src` to `fc-core` and `fc-tests` include dirs)

**Interfaces:**
- Produces: namespace `rate_solver_detail` with `ActiveGroup`, `SolveInputs`, `SeedResult`, `VariableMapping`, `LinearSystem`, `ReducedSystem`, and declarations of the five phase functions (signatures listed in Tasks 3–6).

- [ ] **Step 1: Create the header.**

```cpp
#pragma once

#include "domain/fractional_number.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Node;
struct Link;
struct Pin;

namespace rate_solver_detail
{
    struct ActiveGroup
    {
        std::vector<Pin*> supply;
        std::vector<Pin*> demand;
    };

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
        std::unordered_map<const Pin*, std::pair<std::size_t, FractionalNumber>> associated_variable_index;
        std::size_t num_variables = 0;
        std::vector<std::size_t> group_total_index;
        std::unordered_map<std::size_t, FractionalNumber> group_total_default;
        std::unordered_map<const Pin*, std::pair<std::size_t, bool>> group_membership;
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

    SeedResult SeedRelevantPins(const SolveInputs& in);
    VariableMapping AssignVariables(const SeedResult& seed);
    LinearSystem BuildLinearSystem(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars);
    std::optional<ReducedSystem> ReduceAndCheck(const LinearSystem& sys, const VariableMapping& vars,
                                                float& error_time, float error_flow_duration);
    bool ApplyResults(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars,
                      LinearSystem& sys, ReducedSystem& reduced,
                      float& error_time, float error_flow_duration);
}
```

- [ ] **Step 2: Wire include paths.** In `ficsit-companion/CMakeLists.txt`, after `target_include_directories(fc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)` (~line 95) add:

```cmake
target_include_directories(fc-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

And after `target_include_directories(fc-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)` (~line 223) add:

```cmake
target_include_directories(fc-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

- [ ] **Step 3: Include the header and drop the local `ActiveGroup`.** In `rate_solver.cpp`, add `#include "domain/rate_solver_internal.hpp"` with the other domain includes. Delete the local `struct ActiveGroup { ... };` (current lines 69–73). In `SolveContext`, change the `active_groups` member type to `std::vector<rate_solver_detail::ActiveGroup>` (it stays for now). Add `using namespace rate_solver_detail;` inside the anonymous namespace OR qualify the one type — pick whichever keeps the existing body unchanged with least edits.

- [ ] **Step 4: Reconfigure + build + run suite.**

Run:
```
cmake -S . -B build
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release
```
Expected: builds clean; `100% tests passed ... 196`.

- [ ] **Step 5: Update ledger** (see Progress Tracking section). No commit.

---

### Task 2: Recompose `SolveContext` from the structs (qualify all accesses)

This is the mechanical heart of the refactor: `SolveContext` stops holding loose fields and instead holds the five structs, and every method body is rewritten via the substitution table. Methods stay methods. The compiler verifies completeness.

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Produces (internal only): `SolveContext` now contains `SolveInputs in; float& error_time; float error_flow_duration; SeedResult seed; VariableMapping vars; LinearSystem sys; ReducedSystem reduced;`.

- [ ] **Step 1: Replace `SolveContext`'s data members.** Swap the current loose members (lines ~77–110) for:

```cpp
SolveInputs in;
float& error_time;
float error_flow_duration;

SeedResult seed;
VariableMapping vars;
LinearSystem sys;
ReducedSystem reduced;

void SeedRelevantPins();
void AssignVariables();
void BuildLinearSystem();
bool ReduceAndCheck();
bool ApplyResults();
```

- [ ] **Step 2: Update the `SolveContext` construction** in `RateSolver::Solve` (line ~998) to brace-init the new shape:

```cpp
SolveContext ctx{ { nodes, links, constraint_pin, constraint_value }, error_time, error_flow_duration };
```

- [ ] **Step 3: Apply the substitution table to all five method bodies.** Rewrite every member reference per the table (e.g. `relevant_pins` → `seed.relevant_pins`, `num_variables` → `vars.num_variables`, `constraint_pin` → `in.constraint_pin`, `reduced_matrix` → `reduced.reduced_matrix`). Do NOT change any logic, loop, or arithmetic. The `#if DEBUG_PROPAGATION` blocks reference `relevant_pins`/`multi_pin_constrained` → also qualify to `seed.*`.

- [ ] **Step 4: Build + run suite.**

Run:
```
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release
```
Expected: builds clean; `196` passed. If a member was missed, the compiler errors on the bare name — fix and rebuild.

- [ ] **Step 5: Update ledger.** No commit.

---

### Task 3: Extract `SeedRelevantPins` into `rate_solver_seed.cpp`

**Files:**
- Create: `ficsit-companion/src/domain/rate_solver_seed.cpp`
- Modify: `ficsit-companion/src/domain/rate_solver.cpp` (remove the method; orchestrator calls the free fn)
- Modify: `ficsit-companion/CMakeLists.txt` (`DOMAIN_SOURCE_FILES`)

**Interfaces:**
- Produces: `rate_solver_detail::SeedResult rate_solver_detail::SeedRelevantPins(const SolveInputs& in);`
- Consumes: `SolveInputs` (Task 1).

- [ ] **Step 1: Create `rate_solver_seed.cpp`.** Move into it: the `DEBUG_PROPAGATION` macro + the two `#if DEBUG_PROPAGATION` static globals, the file-local helper `BeltFarEnd` (current lines 39–43), and the body of `SeedRelevantPins`. Wrap the body as:

```cpp
#include "domain/rate_solver_internal.hpp"

#include "domain/graph_item_resolve.hpp"   // IsActiveCargoPin
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/vehicle_route.hpp"

#include <imgui_node_editor.h>

#include <algorithm>
#include <functional>
#include <queue>
#include <set>

#define DEBUG_PROPAGATION 0
// ... (debug statics, BeltFarEnd, then:)

namespace rate_solver_detail
{
    SeedResult SeedRelevantPins(const SolveInputs& in)
    {
        SeedResult seed;
        // <moved body, with substitutions already applied in Task 2>
        return seed;
    }
}
```

Name the local `SeedResult seed;` so the Task-2-qualified `seed.*` accesses are unchanged. `BeltFarEnd` can stay in an anonymous namespace inside this file.

- [ ] **Step 2: Remove from `rate_solver.cpp`.** Delete `SolveContext::SeedRelevantPins`, the `BeltFarEnd` helper, the debug-globals, and the `seed` member is now populated by the orchestrator. In `RateSolver::Solve`, replace `ctx.SeedRelevantPins();` with:

```cpp
ctx.seed = rate_solver_detail::SeedRelevantPins(ctx.in);
```
(`ctx.seed` member stays so the remaining still-method phases keep reading it.)

- [ ] **Step 3: Register the file.** Add `src/domain/rate_solver_seed.cpp` to `DOMAIN_SOURCE_FILES`.

- [ ] **Step 4: Reconfigure + build + run suite.**

Run:
```
cmake -S . -B build
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release
```
Expected: `196` passed.

- [ ] **Step 5: Update ledger.** No commit.

---

### Task 4: Extract `AssignVariables` into `rate_solver_variables.cpp`

**Files:**
- Create: `ficsit-companion/src/domain/rate_solver_variables.cpp`
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

**Interfaces:**
- Produces: `rate_solver_detail::VariableMapping rate_solver_detail::AssignVariables(const SeedResult& seed);`
- Consumes: `SeedResult` (Task 3).

- [ ] **Step 1: Create `rate_solver_variables.cpp`** with includes (`rate_solver_internal.hpp`, `node.hpp`, `pin.hpp`, `recipe.hpp`, `building.hpp`, plus `<cstddef>`) and:

```cpp
namespace rate_solver_detail
{
    VariableMapping AssignVariables(const SeedResult& seed)
    {
        VariableMapping vars;
        // <moved body; reads seed.*, writes vars.*>
        return vars;
    }
}
```
Local named `VariableMapping vars;` keeps the Task-2 `vars.*` accesses intact.

- [ ] **Step 2: Update `rate_solver.cpp`.** Delete `SolveContext::AssignVariables`. In `Solve`, replace `ctx.AssignVariables();` with:

```cpp
ctx.vars = rate_solver_detail::AssignVariables(ctx.seed);
```

- [ ] **Step 3: Register** `src/domain/rate_solver_variables.cpp` in `DOMAIN_SOURCE_FILES`.

- [ ] **Step 4: Reconfigure + build + run suite.** Expected `196` passed.

- [ ] **Step 5: Update ledger.** No commit.

---

### Task 5: Extract `BuildLinearSystem` + `ReduceAndCheck` into `rate_solver_system.cpp`

**Files:**
- Create: `ficsit-companion/src/domain/rate_solver_system.cpp`
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `rate_solver_detail::LinearSystem rate_solver_detail::BuildLinearSystem(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars);`
  - `std::optional<rate_solver_detail::ReducedSystem> rate_solver_detail::ReduceAndCheck(const LinearSystem& sys, const VariableMapping& vars, float& error_time, float error_flow_duration);`
- Consumes: `SolveInputs`, `SeedResult`, `VariableMapping`, `LinearSystem`.

- [ ] **Step 1: Create `rate_solver_system.cpp`** with includes (`rate_solver_internal.hpp`, `linear_solve.hpp`, `node.hpp`, `pin.hpp`, `link.hpp`) and both functions:

```cpp
namespace rate_solver_detail
{
    LinearSystem BuildLinearSystem(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars)
    {
        LinearSystem sys;
        // <moved BuildLinearSystem body; reads in.*, seed.*, vars.*; writes sys.*>
        return sys;
    }

    std::optional<ReducedSystem> ReduceAndCheck(const LinearSystem& sys, const VariableMapping& vars,
                                                float& error_time, float error_flow_duration)
    {
        ReducedSystem reduced;
        reduced.reduced_matrix = ReduceMatrix(sys.equations_coefficients, sys.constants, vars.num_variables);
        // <moved over-determined + inconsistent-row checks; on failure: error_time = error_flow_duration; return std::nullopt;>
        return reduced;
    }
}
```
Note the two return-path changes for `ReduceAndCheck`: the original `return false;` paths become `error_time = error_flow_duration; return std::nullopt;` (they already set `error_time` in Task 2, so just confirm the assignment is present then `return std::nullopt`); the original `return true;` becomes `return reduced;`.

- [ ] **Step 2: Update `rate_solver.cpp`.** Delete both `SolveContext` methods. In `Solve`, replace:

```cpp
ctx.sys = rate_solver_detail::BuildLinearSystem(ctx.in, ctx.seed, ctx.vars);
auto reduced = rate_solver_detail::ReduceAndCheck(ctx.sys, ctx.vars, error_time, error_flow_duration);
if (!reduced) return false;
ctx.reduced = std::move(*reduced);
```
(Keep `ctx.reduced` populated for the still-method `ApplyResults`.)

- [ ] **Step 3: Register** `src/domain/rate_solver_system.cpp` in `DOMAIN_SOURCE_FILES`.

- [ ] **Step 4: Reconfigure + build + run suite.** Expected `196` passed.

- [ ] **Step 5: Update ledger.** No commit.

---

### Task 6: Extract `ApplyResults` into `rate_solver_apply.cpp` and delete `SolveContext`

**Files:**
- Create: `ficsit-companion/src/domain/rate_solver_apply.cpp`
- Modify: `ficsit-companion/src/domain/rate_solver.cpp` (delete `SolveContext`; finalize orchestrator)
- Modify: `ficsit-companion/CMakeLists.txt`

**Interfaces:**
- Produces: `bool rate_solver_detail::ApplyResults(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars, LinearSystem& sys, ReducedSystem& reduced, float& error_time, float error_flow_duration);`
- Consumes: all five structs.

- [ ] **Step 1: Create `rate_solver_apply.cpp`** with includes (`rate_solver_internal.hpp`, `linear_solve.hpp`, `node.hpp`, `pin.hpp`, `link.hpp`, `recipe.hpp`, `building.hpp`, `<cstdio>` for the debug `fprintf`) and:

```cpp
namespace rate_solver_detail
{
    bool ApplyResults(const SolveInputs& in, const SeedResult& seed, const VariableMapping& vars,
                      LinearSystem& sys, ReducedSystem& reduced,
                      float& error_time, float error_flow_duration)
    {
        // <moved ApplyResults body; sys.* and reduced.* mutated in the free-variable loop; in.nodes/in.links iterated>
        return true;
    }
}
```
Keep the `#if DEBUG_PROPAGATION` block; it references `vars.reversed_variable_map` after Task 2 substitution. If that block also references `seed`, it is `seed.*` already.

- [ ] **Step 2: Finalize `rate_solver.cpp`.** Delete the entire `SolveContext` struct and the `ApplyResults` method. The anonymous namespace now contains only `ResetGraphState`. `RateSolver::Solve` becomes:

```cpp
bool RateSolver::Solve(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const Pin* constraint_pin,
                       const FractionalNumber& constraint_value,
                       float& error_time,
                       float error_flow_duration)
{
#if DEBUG_PROPAGATION
    fprintf(stderr, "================================= BEGIN PROPAGATION =================================\n");
    struct OnExit { ~OnExit() { fprintf(stderr, "================================= END PROPAGATION =================================\n"); } } _on_exit;
#endif
    using namespace rate_solver_detail;
    ResetGraphState(nodes, links, error_time);
    SolveInputs in{ nodes, links, constraint_pin, constraint_value };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, error_flow_duration);
    if (!reduced) return false;
    return ApplyResults(in, seed, vars, sys, *reduced, error_time, error_flow_duration);
}
```
Trim now-unused includes from `rate_solver.cpp` (it keeps `node.hpp`, `pin.hpp`, `link.hpp` for `ResetGraphState`, plus `rate_solver_internal.hpp`, `rate_solver.hpp`, `<optional>`, `<memory>`, `<vector>`).

- [ ] **Step 3: Register** `src/domain/rate_solver_apply.cpp` in `DOMAIN_SOURCE_FILES`.

- [ ] **Step 4: Reconfigure + build + run suite.** Expected `196` passed.

- [ ] **Step 5: Verify file sizes.** Confirm no `rate_solver*.cpp` exceeds ~370 lines and `rate_solver.cpp` is now < ~70 lines. Update ledger. No commit.

---

### Task 7: Per-phase unit tests

**Files:**
- Create: `ficsit-companion/tests/test_rate_solver_phases.cpp`
- Modify: `ficsit-companion/CMakeLists.txt` (`TEST_SOURCE_FILES`)

**Interfaces:**
- Consumes: the five `rate_solver_detail` phase functions via `#include "domain/rate_solver_internal.hpp"`, and graph helpers `IdGen` / `MakeLink` from `tests/graph_test_helpers.hpp`.

- [ ] **Step 1: Write the test file.** Reuse the construction patterns from `test_rate_solver.cpp` (build `nodes`/`links` with `IdGen`, `MergerNode`/`CustomSplitterNode`, `MakeLink`). Cover each phase directly. Example anchor tests (add the rest following the same shape):

```cpp
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

#include "domain/fractional_number.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/rate_solver_internal.hpp"

#include "graph_test_helpers.hpp"

using namespace rate_solver_detail;

TEST_CASE("SeedRelevantPins pulls a merger's pins into the relevant set", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);

    REQUIRE(seed.relevant_pins.count(merger->outs[0].get()) == 1);
    // The driven output of a merger is a strongly-constrained multi-pin.
    REQUIRE(seed.multi_pin_constrained.count(merger->outs[0].get()) == 1);
}

TEST_CASE("AssignVariables gives a merger one variable per pin at ratio 1", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);

    // 3 inputs + 1 output = 4 pin variables, each ratio 1.
    REQUIRE(vars.num_variables == 4);
    REQUIRE(vars.associated_variable_index.at(merger->outs[0].get()).second == FractionalNumber(1, 1));
}

TEST_CASE("BuildLinearSystem then ReduceAndCheck solves a lone merger", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);

    // User-constraint equation + merger balance => at least 2 equations.
    REQUIRE(sys.equations_coefficients.size() >= 2);

    float error_time = 0.0f;
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, 1.0f);
    REQUIRE(reduced.has_value());
    REQUIRE(error_time == 0.0f);
}

TEST_CASE("ApplyResults writes back the even split for a lone merger", "[rate_solver][phases]")
{
    IdGen id_gen;
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    nodes.push_back(std::make_unique<MergerNode>(ax::NodeEditor::NodeId(id_gen()), id_gen, nullptr));
    MergerNode* merger = static_cast<MergerNode*>(nodes.back().get());

    SolveInputs in{ nodes, links, merger->outs[0].get(), FractionalNumber(12, 1) };
    const SeedResult seed = SeedRelevantPins(in);
    const VariableMapping vars = AssignVariables(seed);
    LinearSystem sys = BuildLinearSystem(in, seed, vars);
    float error_time = 0.0f;
    std::optional<ReducedSystem> reduced = ReduceAndCheck(sys, vars, error_time, 1.0f);
    REQUIRE(reduced.has_value());

    const bool ok = ApplyResults(in, seed, vars, sys, *reduced, error_time, 1.0f);
    REQUIRE(ok);
    REQUIRE(merger->outs[0]->current_rate == FractionalNumber(12, 1));
    REQUIRE(merger->ins[0]->current_rate == FractionalNumber(4, 1));
}
```

Add at least one failure-path test for `ReduceAndCheck` (an over-constrained system returns `std::nullopt` and sets `error_time`), constructed by locking pins to incompatible totals — mirror an existing rejecting case from `test_rate_solver.cpp`.

- [ ] **Step 2: Register the test file.** Add `tests/test_rate_solver_phases.cpp` to `TEST_SOURCE_FILES`.

- [ ] **Step 3: Reconfigure + build + run the new tests.**

Run:
```
cmake -S . -B build
cmake --build build --config Release --target fc-tests
./build/ficsit-companion/Release/fc-tests.exe "[phases]"
```
Expected: all `[phases]` cases pass.

- [ ] **Step 4: Run the full suite.**

Run: `ctest --test-dir build -C Release`
Expected: all pass (196 + new cases).

- [ ] **Step 5: Coverage gate.** Build Debug, run OpenCppCoverage scoped to `src/domain`, confirm combined `rate_solver*.cpp` line coverage ≥ 86.9% (no regression vs the pre-split single file). Update ledger. No commit.

---

## Progress Tracking

Create `docs/superpowers/plans/2026-06-22-rate-solver-phase-decomposition-PROGRESS.md` at the start of execution with a checkbox per task and a baseline line (full suite count + current `rate_solver.cpp` coverage). After each task: tick its box and append a one-line log entry (suite result + any coverage number). No git operations at any point.

## Self-Review

- **Spec coverage:** structs (Task 1) ✓; named-namespace external linkage (Task 1) ✓; five phase functions with exact signatures (Tasks 3–6) ✓; file layout incl. `BeltFarEnd`/debug statics in seed and `ResetGraphState` in orchestrator (Tasks 3, 6) ✓; `optional` for `ReduceAndCheck` (Task 5) ✓; non-const `LinearSystem&`/`ReducedSystem&` to `ApplyResults` (Task 6) ✓; orchestrator unchanged public API (Task 6) ✓; per-phase tests + registration (Task 7) ✓; acceptance: suite green, coverage ≥86.9%, file-size cap (Tasks 6–7) ✓; no-git (Global Constraints) ✓.
- **Placeholder scan:** method bodies are an explicit verbatim-move + substitution-table operation, not a placeholder; all signatures, structs, orchestrator, CMake edits, and anchor tests are spelled out.
- **Type consistency:** struct/field names and function signatures in Tasks 3–7 match the Task 1 header declarations and the substitution table.
