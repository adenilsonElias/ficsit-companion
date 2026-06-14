# node.cpp + rate_solver.cpp Refactor — PROGRESS

Resume state for the domain refactor. Spec:
`docs/superpowers/specs/2026-06-08-node-and-rate-solver-refactor-design.md`.

## Ground rules
- Work on `main`, **NO git commits** (user manages git).
- Configure/build from repo ROOT: `cmake -S . -B build`; then
  `cmake --build build --config Release` and `ctest --test-dir build -C Release`.
- Decisions locked: node DI = full `INodeDataResolver`; node split = per-family
  (7 .cpp files); `node.hpp` stays a single unified header; rate_solver = no new
  DI, decompose `Solve` only.
- Baseline before this work: **38 tests pass**.

## Part A — node.cpp (DI + per-family split) — DONE
- [x] A1. `domain/node_data_resolver.{hpp,cpp}`: `INodeDataResolver` +
      `GameDataResolver` (wraps `Data::`).
- [x] A2. Threaded `const INodeDataResolver&` through `Node::Deserialize` + all
      deserializing ctors + GroupNode recursion. Injected `GameDataResolver` at
      production_app:370/472 AND session_serializer.cpp:115 (3rd call site).
      Build + 38 tests green.
- [x] A3. `test_node_serialization.cpp` + `FakeNodeDataResolver`: round-trips
      for Merger/CustomSplitter/GameSplitter/Sink/Logistics/Craft/Extractor,
      factory dispatch, unknown-kind throw, unresolvable-recipe throw. 45 tests.
- [x] A4. Split `node.cpp` (1389 lines) → `node_base / craft_node / group_node /
      organizer_nodes / sink_node / logistics_node / extractor_node` (verbatim
      slice). node.cpp removed; CMake DOMAIN_SOURCE_FILES updated. 45 tests
      green; app links. **node.hpp kept unified.**

## Part B — rate_solver.cpp (decompose Solve) — DONE (revised approach)
- [x] B1. Extended `test_rate_solver.cpp` with 3 CraftNode topologies (output
      driven, input driven, craft→merger ratio across a link) — captures the
      previously-untested ratio-variable path. 48 tests green.
- [x] B2. **Deviated from the SolverContext 7-phase plan** — see note below.
      Extracted the two cleanly-separable pure kernels:
      - `domain/linear_solve.{hpp,cpp}` `ReduceMatrix(coeffs, constants,
        num_variables)` — the Gaussian-elimination kernel, now an independently
        unit-tested free function (`test_linear_solve.cpp`, 4 tests). Verbatim
        except the two inert `#if DEBUG_PROPAGATION` print blocks were dropped.
      - File-local `ResetGraphState(nodes, links, error_time)` helper.
      `Solve`: 839 → 734 lines; 3 call sites now use `ReduceMatrix` (incl. the 2
      inside the fixpoint loop). 52 tests green; app links.

### Why B2 deviated from the spec
Reading the full `Solve` showed the back half (free-variable resolution) is a
**fixpoint loop that augments the equations and RE-reduces the matrix** (it
calls the reducer 3×), not a linear pipeline. The spec's "7 sequential phases"
model (derived from comment headers) does not match that control flow, so a
forced phase split would have raised regression risk on the most delicate code
for little gain. Instead I isolated the genuinely self-contained pure kernels
(matrix reduction + reset) and gave the numeric kernel real unit tests — the
testability win the user asked for — while leaving the tightly-coupled
collect/build/solve loop intact and guarded by the extended end-to-end tests.

## Coverage follow-ups (after Parts A & B)
- [x] GroupNode serialization round-trip test (recursive resolver path) —
      group_node 0% → 41%.
- [x] GroupNode rate-logic tests (`test_group_node.cpp`): UpdateRate /
      PropagateRateToSubnodes / ComputePowerUsage / UpdateDetails / CreateInsOuts
      / SetBuiltState — group_node → 67.4%.
- Final suite: **56 tests, all green.** Coverage (excl. third-party
  `include/third_party/stb_image.h`, 3828 lines @ 0%): **62.3%** (1929/3097).
  Module highlights: linear_solve 100%, craft_node 91%, node_base 89%,
  rate_solver 81%, group_node 67%.

### Known remaining gaps (not done — optional future work)
- `group_node.cpp` (~33% left): heterogeneous-subnode branches — nested
  group-in-group, and Sink/Extractor/Logistics subnode cases in
  PropagateRateToSubnodes / UpdateDetails / CreateInsOuts. Need richer fixtures.
- `node_data_resolver.cpp` `GameDataResolver` impl: wraps global `Data::`, only
  covered by manual/integration, not units (by design).
- The rate_solver fixpoint-loop tail (weak-constraint / free-variable paths)
  ~19% uncovered.

## Status log
- 2026-06-08: spec approved; PROGRESS created.
- 2026-06-08: Part A DONE (resolver DI + 7-file split, 45 tests).
- 2026-06-08: Part B DONE (linear_solve kernel + reset extracted, 52 tests).
  Deviated from SolverContext plan — see note above.
- 2026-06-08: Coverage follow-ups DONE (group serialize + group rate logic,
  56 tests, 62.3% excl. stb_image). **All work for this effort complete.**
  No git commits made — user manages git. Build/test all green.
