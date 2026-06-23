# Rate Solver Refactor + Domain Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the 887-line `RateSolver::Solve` into a `SolveContext` with one method per phase (behavior-preserving), then raise every sub-70% domain `.cpp` file toward ~80% line coverage.

**Architecture:** Phase 1 introduces a private `SolveContext` struct in `rate_solver.cpp`'s anonymous namespace that owns the solve's working state; `Solve` becomes a thin orchestrator calling `SeedRelevantPins → AssignVariables → BuildLinearSystem → ReduceAndCheck → ApplyResults`. The public header (`rate_solver.hpp`) is unchanged. Phase 2 adds Catch2 tests for the low-coverage domain files; no production behavior changes.

**Tech Stack:** C++17, Catch2, CMake, OpenCppCoverage (Windows/MSVC), ImGui Node Editor types.

## Global Constraints

- **No git commits or branches** unless the user explicitly asks. Instead, after each task tick its checkbox and append a line to `docs/superpowers/plans/2026-06-20-rate-solver-refactor-and-domain-coverage-PROGRESS.md`. (Standing user preference — overrides the skill's "commit" steps.)
- **Refactor (Phase 1) is strictly behavior-preserving.** No change to numeric results, error paths, return values, or `error_time` semantics.
- **Public API unchanged:** `rate_solver.hpp` keeps exactly `static bool RateSolver::Solve(nodes, links, constraint_pin, constraint_value, error_time, error_flow_duration);`.
- Preserve all `#if DEBUG_PROPAGATION` scaffolding verbatim, inside whichever phase its code currently lives.
- Build/test per `CLAUDE.md`. Release suite must stay green (136/136). Coverage measured via a single Debug `fc-tests` run + OpenCppCoverage (the prior Debug full-suite segfault is resolved).
- `SolveContext`, `ActiveGroup`, `BeltFarEnd`, and `ResetGraphState` all live in the anonymous namespace of `rate_solver.cpp` — never exposed in a header.

## Build & test commands (reference)

```bash
# Configure once
cmake -DCMAKE_BUILD_TYPE=Release -S ficsit-companion -B build   # (or per CLAUDE.md root config)

# Build + run the test suite (Release) — the green-bar gate for every refactor task
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure

# Run only the rate-solver tests during the refactor (faster inner loop)
./build/ficsit-companion/Release/fc-tests.exe "[rate_solver]"
```

Coverage (Phase 2), per `CLAUDE.md` (PowerShell):

```powershell
cmake --build build --config Debug --target fc-tests
$repo = (Get-Location).Path
OpenCppCoverage `
  --sources "$repo\ficsit-companion\src\domain" `
  --sources "$repo\ficsit-companion\include\domain" `
  --excluded_sources "$repo\build" `
  --export_type cobertura:coverage-domain.xml `
  --export_type html:coverage-html `
  -- "$repo\build\ficsit-companion\Debug\fc-tests.exe"
```

---

# Phase 1 — `rate_solver` decomposition

> Each task below is a mechanical extraction of an existing, already-tested code
> region. The "code" for each move is the **existing** body of `Solve` for the
> stated line range — relocate it verbatim, changing only local-variable
> references to `SolveContext` members (`x` → `this->x`/`x` as a member). Do not
> alter logic. The test cycle for every task is: suite green before → make the
> move → suite green after.

## Task 1: Introduce `SolveContext` and route Solve's state through it

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`
- Test (existing, unchanged): `ficsit-companion/tests/test_rate_solver.cpp`

**Interfaces:**
- Produces: a `SolveContext` struct (anonymous namespace) holding all cross-phase state, with members named exactly as below. Later tasks add methods to it.

- [ ] **Step 1: Confirm the safety net is green**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release --output-on-failure`
Expected: PASS (136/136). If not, stop — the refactor needs a green baseline.

- [ ] **Step 2: Add the `ActiveGroup` + `SolveContext` declarations**

In `rate_solver.cpp`, move the existing `ActiveGroup` struct (currently declared inside `Solve`, lines ~115–119) up into the anonymous namespace (next to `BeltFarEnd`/`ResetGraphState`), then add the context struct immediately after it:

```cpp
namespace
{
    struct ActiveGroup
    {
        std::vector<Pin*> supply;
        std::vector<Pin*> demand;
    };

    struct SolveContext
    {
        // --- inputs ---
        std::vector<std::unique_ptr<Node>>& nodes;
        std::vector<std::unique_ptr<Link>>& links;
        const Pin* constraint_pin;
        const FractionalNumber& constraint_value;
        float& error_time;
        float error_flow_duration;

        // --- phase 1: seeding / collection ---
        std::unordered_set<const Pin*> relevant_pins;
        std::unordered_set<const Pin*> multi_pin_constrained;
        std::vector<ActiveGroup> active_groups;

        // --- phase 2: variable assignment ---
        std::unordered_map<const Pin*, std::pair<size_t, FractionalNumber>> associated_variable_index;
        size_t num_variables = 0;
        std::vector<size_t> group_total_index;
        std::unordered_map<size_t, FractionalNumber> group_total_default;
        std::unordered_map<const Pin*, std::pair<size_t, bool>> group_membership; // bool: is_supply
        std::vector<const Pin*> reversed_variable_map;

        // --- phase 3: linear system ---
        std::vector<std::vector<FractionalNumber>> equations_coefficients;
        std::vector<FractionalNumber> constants;

        // --- phase 4: reduction ---
        std::vector<std::vector<FractionalNumber>> reduced_matrix;

        // --- phase 5: results ---
        std::vector<FractionalNumber> solution;

        // Methods added in later tasks:
        void SeedRelevantPins();   // Task 2
        void AssignVariables();    // Task 3
        void BuildLinearSystem();  // Task 4
        bool ReduceAndCheck();     // Task 5
        bool ApplyResults();       // Task 6
    };
}
```

- [ ] **Step 3: Construct the context at the top of `Solve` and delete the now-duplicated local declarations**

In `Solve`, after the existing `ResetGraphState(nodes, links, error_time);` call, add:

```cpp
    SolveContext ctx{ nodes, links, constraint_pin, constraint_value, error_time, error_flow_duration };
```

Then delete the in-function declarations of every variable now living in `SolveContext` (`relevant_pins`, `multi_pin_constrained`, `active_groups`, `associated_variable_index`, `num_variables`, `group_total_index`, `group_total_default`, `group_membership`, `reversed_variable_map`, `equations_coefficients`, `constants`, `reduced_matrix`, `solution`, and the moved `ActiveGroup`). Leave the transient locals in place (`pins_to_propagate`, `multi_pin_maybe_constrained`, `processed_groups`, `processed_links`, `processed_multi_node`, `processed_pins`) — they do not cross phases.

- [ ] **Step 4: Prefix all references to the moved variables with `ctx.`**

Throughout the remaining body of `Solve`, replace bare references to the moved variables with `ctx.` (e.g. `relevant_pins` → `ctx.relevant_pins`, `num_variables` → `ctx.num_variables`). The `create_variable` lambda captures `[&]`, so it now reads `ctx.associated_variable_index` / `ctx.num_variables` — update those references inside the lambda too. Logic is otherwise untouched.

- [ ] **Step 5: Build and run the rate-solver tests**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[rate_solver]"`
Expected: PASS. Then run the full suite: `ctest --test-dir build -C Release --output-on-failure` → 136/136.

- [ ] **Step 6: Record progress**

Tick this task in the plan and append to `…-PROGRESS.md`: `Task 1 done — SolveContext introduced, state routed through ctx, suite green (136/136).`

---

## Task 2: Extract `SeedRelevantPins()`

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Consumes: `ctx.constraint_pin`, `ctx.nodes`, `ctx.links`.
- Produces: populates `ctx.relevant_pins`, `ctx.multi_pin_constrained`, `ctx.active_groups`; sets link flow.

- [ ] **Step 1: Move the seeding/collection region into the method**

Cut the body that currently runs from the first propagation seeding (the `pins_to_propagate` setup, ~line 93) through the end of the collection `while` loop and its trailing `#if DEBUG_PROPAGATION` block (~line 303) out of `Solve` and into `SolveContext::SeedRelevantPins()`. The transient locals (`pins_to_propagate`, `multi_pin_maybe_constrained`, `processed_groups`) move with it as locals of the method. `BeltFarEnd` is already a free function and stays callable. References to `constraint_pin` become `this->constraint_pin`.

```cpp
void SolveContext::SeedRelevantPins()
{
    // ... moved body (queues seeded from constraint_pin, the collection while-loop,
    //     per-node-type propagation, vehicle-route group expansion, DEBUG block) ...
}
```

- [ ] **Step 2: Call it from `Solve`**

Replace the removed region in `Solve` with `ctx.SeedRelevantPins();` (placed right after the `ctx` construction).

- [ ] **Step 3: Build + test**

Run: `cmake --build build --config Release --target fc-tests && ./build/ficsit-companion/Release/fc-tests.exe "[rate_solver]"`
Expected: PASS. Full suite: `ctest --test-dir build -C Release --output-on-failure` → 136/136.

- [ ] **Step 4: Record progress** — tick checkbox, append `Task 2 done` line to PROGRESS.md.

---

## Task 3: Extract `AssignVariables()`

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Consumes: `ctx.relevant_pins`, `ctx.multi_pin_constrained`, `ctx.active_groups`, `ctx.constraint_pin`.
- Produces: `ctx.associated_variable_index`, `ctx.num_variables`, `ctx.group_total_index`, `ctx.group_total_default`, `ctx.group_membership`, `ctx.reversed_variable_map`.

- [ ] **Step 1: Move the variable-assignment region into the method**

Cut from the `create_variable` lambda (~line 305) through the construction of `reversed_variable_map` (~line 443) into `SolveContext::AssignVariables()`. The `create_variable` lambda becomes a local lambda inside the method (still capturing `[&]`, now referring to `this`-qualified members) — or, optionally, a private member function `CreateVariable(const Pin*)`; either is acceptable, lambda keeps the diff smaller.

```cpp
void SolveContext::AssignVariables()
{
    auto create_variable = [&](const Pin* pin) { /* moved body */ };
    // strongly-constrained pass, remaining pass, auxiliary T allocation,
    // group_membership + reversed_variable_map construction
}
```

- [ ] **Step 2: Call it from `Solve`** — replace the removed region with `ctx.AssignVariables();`.

- [ ] **Step 3: Build + test** — `[rate_solver]` then full suite. Expected: PASS / 136/136.

- [ ] **Step 4: Record progress** — tick checkbox, append `Task 3 done` line.

---

## Task 4: Extract `BuildLinearSystem()`

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Consumes: `ctx.relevant_pins`, `ctx.associated_variable_index`, `ctx.num_variables`, `ctx.active_groups`, `ctx.group_total_index`, `ctx.constraint_pin`, `ctx.constraint_value`.
- Produces: `ctx.equations_coefficients`, `ctx.constants`.

- [ ] **Step 1: Move the equation-building region into the method**

Cut from the first (user-constraint) equation (~line 445) through the end of the per-route-group balance loop (~line 549) into `SolveContext::BuildLinearSystem()`. The transient locals `processed_links` and `processed_multi_node` move with it.

```cpp
void SolveContext::BuildLinearSystem()
{
    // user constraint equation, per-link equality, merger/splitter balance,
    // per-route-group balance equations
}
```

- [ ] **Step 2: Call it from `Solve`** — replace the removed region with `ctx.BuildLinearSystem();`.

- [ ] **Step 3: Build + test** — `[rate_solver]` then full suite. Expected: PASS / 136/136.

- [ ] **Step 4: Record progress** — tick checkbox, append `Task 4 done` line.

---

## Task 5: Extract `ReduceAndCheck()`

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Consumes: `ctx.equations_coefficients`, `ctx.constants`, `ctx.num_variables`, `ctx.error_time`, `ctx.error_flow_duration`.
- Produces: `ctx.reduced_matrix`; returns `false` (and sets `error_time = error_flow_duration`) when the system has no solution.

- [ ] **Step 1: Move the reduction + consistency checks into the method**

Cut from the `ReduceMatrix(...)` call (~line 551) through the inconsistent-row check that ends at ~line 591 into `SolveContext::ReduceAndCheck()`. Convert the two `return false;` paths to `return false;` from the method (they already set `error_time`). End the method with `return true;`.

```cpp
bool SolveContext::ReduceAndCheck()
{
    reduced_matrix = ReduceMatrix(equations_coefficients, constants, num_variables);
    // over-determined all-zero-rows check  -> return false on violation
    // inconsistent-row check               -> return false on violation
    return true;
}
```

- [ ] **Step 2: Call it from `Solve`**

```cpp
    if (!ctx.ReduceAndCheck()) return false;
```

- [ ] **Step 3: Build + test** — `[rate_solver]` then full suite. Expected: PASS / 136/136.

- [ ] **Step 4: Record progress** — tick checkbox, append `Task 5 done` line.

---

## Task 6: Extract `ApplyResults()` and finalize the orchestrator

**Files:**
- Modify: `ficsit-companion/src/domain/rate_solver.cpp`

**Interfaces:**
- Consumes: `ctx.reduced_matrix`, `ctx.equations_coefficients`, `ctx.constants`, `ctx.num_variables`, `ctx.associated_variable_index`, `ctx.reversed_variable_map`, `ctx.group_membership`, `ctx.group_total_index`, `ctx.group_total_default`, `ctx.active_groups`, `ctx.nodes`, `ctx.links`, `ctx.error_time`, `ctx.error_flow_duration`.
- Produces: writes `current_rate` back to pins, sets link flow, calls `UpdateRate`; returns `false` on error/negative solution, `true` on success.

- [ ] **Step 1: Move the free-variable resolution + back-substitution + apply region into the method**

Cut from `std::unordered_set<const Pin*> processed_pins;` (~line 593) through the final `return true;` (~line 956) into `SolveContext::ApplyResults()`. The transient local `processed_pins` and the free-variable `while(true)` loop, back-substitution, negative-solution check, link-flow check, and rate-application loop all move together. Preserve every `error_time = error_flow_duration; return false;` path and the trailing `return true;`.

```cpp
bool SolveContext::ApplyResults()
{
    std::unordered_set<const Pin*> processed_pins;
    // free-variable resolution loop (uses group_membership/group_total_* + ReduceMatrix re-runs)
    // back substitution -> solution
    // negative-solution check (sets pin->error, error_time) -> return false if error_time > 0
    // link-flow keep/clear pass
    // write current_rate + UpdateRate per node kind
    return true;
}
```

- [ ] **Step 2: Reduce `Solve` to the orchestrator**

After this task, `Solve` should read essentially:

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
    ResetGraphState(nodes, links, error_time);
    SolveContext ctx{ nodes, links, constraint_pin, constraint_value, error_time, error_flow_duration };
    ctx.SeedRelevantPins();
    ctx.AssignVariables();
    ctx.BuildLinearSystem();
    if (!ctx.ReduceAndCheck()) return false;
    return ctx.ApplyResults();
}
```

- [ ] **Step 3: Build + test** — `[rate_solver]` then full suite. Expected: PASS / 136/136.

- [ ] **Step 4: Verify coverage didn't regress**

Run the OpenCppCoverage command (see reference block). Confirm `rate_solver.cpp` line coverage is ≥86.6%.
Expected: ≥86.6% (no logic lost).

- [ ] **Step 5: Record progress** — tick checkbox, append `Task 6 done — Solve() now ~15-line orchestrator, rate_solver coverage <value>%.`

---

# Phase 2 — Domain coverage push

> Target: each file toward **~80%** line coverage; overall domain line coverage
> above 72.2%. **Tests only — no production changes.** ImGui rendering functions
> (`Recipe::Render`, `FractionalNumber::RenderInputText`) require a live ImGui
> context and are out of scope for unit tests — exclude them when judging "done"
> (a file dominated by such code may cap below 80%; note it rather than chase it).
>
> Each task's loop: (1) read the file + the existing `test_<file>.cpp` if any;
> (2) run the coverage command to see which lines/branches are cold;
> (3) add Catch2 cases exercising the cold logic; (4) re-run coverage and record
> before/after. Order: lowest coverage first (biggest wins).

## Task 7: `vehicle_map.cpp` — 8.2% → ~80%

**Files:**
- Modify/Create test: `ficsit-companion/tests/test_vehicle_map_import.cpp` (extend existing)
- Read: `ficsit-companion/src/domain/vehicle_map.cpp`, `ficsit-companion/include/domain/vehicle_map.hpp`

**Public API to cover:** `VehicleMap::ParseLogisticsJson(const std::string&) -> Model`, `VehicleMap::ExtractLogisticsWarnings(const std::string&) -> std::vector<std::string>`, `VehicleMap::VehicleTypeName(VehicleType) -> const char*`, plus the internal `ToLower`/`ParseStationKind`/`ParseVehicleType`/`ReadPos` reached through `ParseLogisticsJson`.

- [x] **Step 1: Read the file and run baseline coverage** — note which functions/branches are uncovered. Expected baseline ~8.2%.

- [x] **Step 2: Add fixture-driven parse tests**

Write Catch2 cases under the `[vehicle_map]` tag feeding representative wrapper-JSON strings to `ParseLogisticsJson`, asserting on the resulting `Model` (stations, vehicles, segments, item refs). Cover: a well-formed multi-station/train + truck payload; each `VehicleType` string (`tractor`, `explorer`, `cyberwagon`, `train`, `truck`, and an unknown → `VehicleType::Unknown`); `StationKind` train vs truck; missing/short `pos` arrays (exercise `ReadPos` defaults); and `Model::Empty()` true on empty input. Example shape:

```cpp
TEST_CASE("ParseLogisticsJson reads stations and vehicle types", "[vehicle_map]")
{
    const std::string wrapper = R"({ /* representative logistics json */ })";
    const VehicleMap::Model m = VehicleMap::ParseLogisticsJson(wrapper);
    REQUIRE_FALSE(m.Empty());
    REQUIRE(m.stations.size() == /* expected */);
    // assert station kind, vehicle type mapping, positions, segment path kind
}
```

Build the fixture JSON from the actual shape `ParseLogisticsJson` expects (read the parser to get exact keys). Reuse any existing import fixture in the repo's tools/test data if present.

- [x] **Step 3: Add warnings + name tests**

Cover `ExtractLogisticsWarnings` (input that produces ≥1 warning and input that produces none) and `VehicleTypeName` for every enum value.

- [x] **Step 4: Build + run** — `./build/ficsit-companion/Release/fc-tests.exe "[vehicle_map]"`. Expected: PASS.

- [x] **Step 5: Re-run coverage; record before/after** — target ≥~80% (excluding any unreachable error branches). Append `Task 7: vehicle_map 8.2% → <value>%` to PROGRESS.md.

---

## Task 8: `graph_item_resolve.cpp` — 35.6% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_graph_item_resolve.cpp`
- Read: `ficsit-companion/src/domain/graph_item_resolve.cpp`, `include/domain/graph_item_resolve.hpp`

**Public API to cover:** `IsFuelPin`, `IsVehiclePlug`, `IsActiveCargoPin`, `ResolveItemThroughChain(Pin*)`, `ResolveOrganizerItem(Node*)`, `RecalculateOrganizerItemChain(OrganizerNode*)`, `PropagateExtractorResourceUpstream(Node*, const Item*, id_generator)`.

- [x] **Step 1: Read file + baseline coverage** — identify cold predicates/branches.

- [x] **Step 2: Build minimal graph fixtures and test the predicates + resolvers**

Construct small node/pin graphs in-test (follow the construction patterns already used in `test_rate_solver.cpp` / organizer tests) and assert: each predicate true and false case; `ResolveItemThroughChain` across a multi-hop organizer chain and at a dead end (nullptr item); `ResolveOrganizerItem`; `RecalculateOrganizerItemChain` updates the chain; `PropagateExtractorResourceUpstream` sets items upstream using a stub `id_generator` returning incrementing ids.

```cpp
TEST_CASE("ResolveItemThroughChain follows organizer hops", "[graph_item_resolve]")
{
    // build: extractor -> organizer -> input_pin
    REQUIRE(ResolveItemThroughChain(input_pin) == expected_item);
}
```

- [x] **Step 3: Build + run** — `"[graph_item_resolve]"`. Expected: PASS.

- [x] **Step 4: Re-run coverage; record** — target ≥~80%. Append PROGRESS line.

---

## Task 9: `recipe.cpp` — 45.8% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_recipe.cpp`
- Read: `ficsit-companion/src/domain/recipe.cpp`, `include/domain/recipe.hpp`

**Public API to cover (non-ImGui):** `Recipe::FindInName(const std::string&)`, `Recipe::FindInIngredients(const std::string&)`, plus `Item`/`CountedItem` construction/fields. **`Recipe::Render` is ImGui — out of scope.**

- [x] **Step 1: Read file + baseline coverage** — confirm how much of the 45.8% gap is `Render` (UI, excluded) vs. `FindInName`/`FindInIngredients` (testable). Note the realistic ceiling if `Render` dominates.

- [x] **Step 2: Test the search helpers**

```cpp
TEST_CASE("Recipe::FindInName matches case-insensitively / returns npos", "[recipe]")
{
    Recipe r = /* construct with a known display name + ingredients */;
    REQUIRE(r.FindInName("substr") != std::string::npos);
    REQUIRE(r.FindInName("nope")   == std::string::npos);
    REQUIRE(r.FindInIngredients("iron") != std::string::npos);
}
```

Cover match, no-match, and case/edge behavior matching the implementation.

- [x] **Step 3: Build + run** — `"[recipe]"`. Expected: PASS.

- [x] **Step 4: Re-run coverage; record** — target ≥~80% of non-`Render` lines. Append PROGRESS line (note `Render` exclusion if relevant).

---

## Task 10: `pin.cpp` — 50.9% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_pin.cpp`
- Read: `ficsit-companion/src/domain/pin.cpp`, `include/domain/pin.hpp`

**Public API to cover:** `Pin::SetLocked(bool)`, `Pin::GetLocked()`, and the remaining pin construction / rate / item / link-state members exercised in `pin.cpp`.

- [x] **Step 1: Read file + baseline coverage** — list cold member functions.

- [x] **Step 2: Test lock state + the other uncovered members**

```cpp
TEST_CASE("Pin lock state round-trips", "[pin]")
{
    Pin p = /* construct */;
    REQUIRE_FALSE(p.GetLocked());
    p.SetLocked(true);
    REQUIRE(p.GetLocked());
}
```

Add cases for every other non-trivial method shown cold by coverage (construction, rate accessors, item assignment), asserting observable behavior.

- [x] **Step 3: Build + run** — `"[pin]"`. Expected: PASS.

- [x] **Step 4: Re-run coverage; record** — target ≥~80%. Append PROGRESS line.

---

## Task 11: `graph_model.cpp` — 62.4% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_graph_model.cpp`
- Read: `ficsit-companion/src/domain/graph_model.cpp`, `include/domain/graph_model.hpp`

**Public API to cover:** `GraphModel::GetNextId()`, `FindPin(PinId)`, `CreateLink(start, end, trigger_update, error_time, error_flow_duration)`, `DeleteLink(LinkId)`, `DeleteNode(NodeId)`. `GraphModel` takes an `IEditorBackend&` — check for an existing fake/stub backend in the test sources and reuse it; if none, create a minimal `IEditorBackend` test double.

- [x] **Step 1: Read file + baseline coverage + locate/create a fake `IEditorBackend`.**

- [x] **Step 2: Test id generation, pin lookup, link create/delete, node delete**

```cpp
TEST_CASE("GraphModel CreateLink then DeleteLink", "[graph_model]")
{
    FakeEditorBackend backend;
    GraphModel model(backend);
    // add nodes/pins via model, then:
    float err = 0.0f;
    model.CreateLink(start, end, /*trigger_update*/ false, err, 1.0f);
    REQUIRE(/* link exists */);
    model.DeleteLink(linkId);
    REQUIRE(/* link gone */);
}
```

Cover `GetNextId` monotonicity, `FindPin` hit + miss (nullptr), `CreateLink` with and without `trigger_update`, `DeleteLink`, and `DeleteNode` (removing incident links).

- [x] **Step 3: Build + run** — `"[graph_model]"`. Expected: PASS.

- [x] **Step 4: Re-run coverage; record** — target ≥~80%. Append PROGRESS line.

---

## Task 12: `json.cpp` — 66.3% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_json.cpp`
- Read: `ficsit-companion/src/domain/json.cpp`, `include/domain/json.hpp`

**Public API to cover:** `Json::Value` type predicates (`is_null/string/object/array/bool/integer/number`), typed getters (`get_object/get_array/get_string`, `get<T>`, `get_number<T>`), `contains`, `size`, `push_back`, parsing (round-trip parse → `Dump`), and `Dump` with/without indent. Focus on the branches coverage shows cold (error/edge paths: wrong-type access, missing key, malformed input if the parser surfaces it).

- [ ] **Step 1: Read file + baseline coverage** — list cold branches (often the type-mismatch and pretty-print paths).

- [ ] **Step 2: Add round-trip + accessor + edge tests**

```cpp
TEST_CASE("Json round-trips objects and arrays", "[json]")
{
    const std::string src = R"({"a":1,"b":[true,null,"x"],"c":3.5})";
    Json::Value v = /* parse src */;
    REQUIRE(v.is_object());
    REQUIRE(v.contains("a"));
    REQUIRE(v.get_object().size() == 3);
    REQUIRE(v["b"].is_array());
    REQUIRE(v["b"].size() == 3);
    const std::string out = v.Dump();          // compact
    const std::string pretty = v.Dump(2);      // indented branch
    REQUIRE_FALSE(out.empty());
    REQUIRE(pretty.find('\n') != std::string::npos);
}
```

Add: each `is_*` true/false; typed-getter happy paths; a wrong-type access path; `push_back` to an array; integer vs number distinction.

- [ ] **Step 3: Build + run** — `"[json]"`. Expected: PASS.

- [ ] **Step 4: Re-run coverage; record** — target ≥~80%. Append PROGRESS line.

---

## Task 13: `fractional_number.cpp` — 67.9% → ~80%

**Files:**
- Create/extend test: `ficsit-companion/tests/test_fractional_number.cpp`
- Read: `ficsit-companion/src/domain/fractional_number.cpp`, `include/domain/fractional_number.hpp`

**Public API to cover (non-ImGui):** construction from string/expression (the postfix expression parser at the top of the file), `GetNumerator/GetDenominator/GetValue`, `GetStringFraction/GetStringFloat`, `Simplify`, `UpdateValue`, and arithmetic operators. **`RenderInputText` is ImGui — out of scope.**

- [ ] **Step 1: Read file + baseline coverage** — the cold region is typically the expression-parser branches (operators, decimals, parentheses, malformed input).

- [ ] **Step 2: Test the expression parser + arithmetic + simplification**

```cpp
TEST_CASE("FractionalNumber parses expressions exactly", "[fractional_number]")
{
    REQUIRE(FractionalNumber("1/3").GetNumerator() == 1);
    REQUIRE(FractionalNumber("1/3").GetDenominator() == 3);
    REQUIRE(FractionalNumber("2.5").GetValue() == Approx(2.5));
    REQUIRE(FractionalNumber("1/2 + 1/2").GetNumerator() == 1);   // == 1/1
    REQUIRE(FractionalNumber("4/8").GetDenominator() == 2);        // simplified
    // exercise operators *, /, +, - and parentheses if supported
}
```

Cover: integer, fraction, decimal, multi-operator expressions, parentheses, simplification (`4/8 → 1/2`), and a malformed/edge input path. Add operator tests (`+ - * /`) and `GetStringFraction`/`GetStringFloat` formatting.

- [ ] **Step 3: Build + run** — `"[fractional_number]"`. Expected: PASS.

- [ ] **Step 4: Re-run coverage; record** — target ≥~80% of non-`RenderInputText` lines. Append PROGRESS line.

---

## Task 14: Refresh the coverage report

**Files:**
- Create: `docs/domain-coverage-report-2026-06-20.md`

- [ ] **Step 1: Run a clean full-suite coverage pass** (Debug `fc-tests`, OpenCppCoverage over `src/domain` + `include/domain`).

- [ ] **Step 2: Regenerate the per-file table** in the same format as `docs/domain-coverage-report-2026-06-18.md`, with new percentages and the new overall domain figure. Note the `rate_solver` decomposition (now multiple methods, still ≥86.6%) and the coverage gains per file. Drop the "Debug-only full-suite crash" finding (resolved) or mark it resolved.

- [ ] **Step 3: Record progress** — tick final checkbox; append `Task 14 done — refreshed coverage report, overall domain <old>% → <new>%.`

---

## Self-Review (completed during planning)

- **Spec coverage:** Workstream 1 (decomposition) → Tasks 1–6; Workstream 2 (coverage for all sub-70% files) → Tasks 7–13 (vehicle_map, graph_item_resolve, recipe, pin, graph_model, json, fractional_number — all seven sub-70% files); refreshed report → Task 14. Sequencing (refactor first) honored. No-git / PROGRESS.md preference encoded in Global Constraints and every task's last step.
- **Type consistency:** `SolveContext` member names and the five method signatures (`SeedRelevantPins`, `AssignVariables`, `BuildLinearSystem`, `ReduceAndCheck`, `ApplyResults`) are defined in Task 1 and reused verbatim in Tasks 2–6.
- **Known caveat (not a placeholder):** Tasks 7–13 deliberately defer exact test assertions to "after reading the file + cold-line coverage," because precise assertions depend on each function's runtime behavior; each task pins down the API surface, an example test, the run command, and the measurable ≥~80% target. ImGui `Render*` exclusions are called out where they cap the achievable percentage.
