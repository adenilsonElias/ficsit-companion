# node.cpp + rate_solver.cpp Refactor — Design

**Date:** 2026-06-08
**Status:** Approved (pending spec review)

Two sequential sub-projects in `src/domain/`, each a pure refactor with its own
build + `ctest` checkpoints. **No git commits** (user manages git). Continues the
DI + Catch2 pattern of the prior refactors.

---

## Part A — `node.cpp` (1,411 lines): per-family split + resolver DI

### Problem
A single TU holds the whole polymorphic hierarchy (11 classes) and, during
deserialization, reaches into the **global `Data::Recipes()` / `Data::Items()`
singleton** (node.cpp:173, 754, 1017, 1098, 1114, 1261). That global coupling is
why tests can only build nodes with `nullptr` recipes/items and never exercise
JSON deserialization.

### A1. Resolver DI (full)
New `domain/node_data_resolver.hpp/cpp`:

```cpp
struct INodeDataResolver
{
    virtual ~INodeDataResolver() = default;
    virtual const Recipe* FindRecipe(const std::string& name) const = 0;
    virtual const Item*   FindItem(const std::string& name) const = 0;
};

// Production impl, wraps the Data:: singleton.
class GameDataResolver : public INodeDataResolver
{
    const Recipe* FindRecipe(const std::string& name) const override; // find_if over Data::Recipes()
    const Item*   FindItem(const std::string& name) const override;   // Data::Items().find()
};
```

Thread `const INodeDataResolver&` through the deserialization path, replacing
every direct `Data::` call:

- `Node::Deserialize(id, id_generator, serialized, resolver)` — new last param.
- Each **deserializing** constructor gains a trailing `const INodeDataResolver&`
  (`CraftNode`, `OrganizerNode` and its subclasses `CustomSplitter`/`Merger`/
  `GameSplitter`, `SinkNode`, `LogisticsNode`, `ExtractorNode`, `GroupNode`).
- `GroupNode`'s **recursive** subnode deserialization (node.cpp:320) forwards the
  resolver.
- Non-deserializing constructors (e.g. `CraftNode(id, const Recipe*, idgen)`) are
  unchanged — they already receive resolved pointers.

Call sites inject a `GameDataResolver` (behaviorally identical to today):
- `production_app.cpp:370` (load session) and `:472` (duplicate/paste).

### A2. Per-family file split
Split `src/domain/node.cpp` into seven TUs; **`include/domain/node.hpp` stays a
single unified header** (the inheritance chain is tightly coupled; one header is
the coherent unit and avoids include churn at the ~dozens of call sites that do
`#include "domain/node.hpp"`).

| New file | Classes |
|---|---|
| `node_base.cpp` | `Node`, `PoweredNode`, `Node::Deserialize` factory |
| `craft_node.cpp` | `CraftNode` |
| `group_node.cpp` | `GroupNode` (~450 lines: CreateInsOuts, PropagateRateToSubnodes, UpdateDetails) |
| `organizer_nodes.cpp` | `OrganizerNode`, `CustomSplitterNode`, `MergerNode`, `GameSplitterNode` |
| `sink_node.cpp` | `SinkNode` |
| `logistics_node.cpp` | `LogisticsNode` |
| `extractor_node.cpp` | `ExtractorNode` |

The old `src/domain/node.cpp` is removed; all seven (plus
`node_data_resolver.cpp`) added to `DOMAIN_SOURCE_FILES`.

### A3. Tests — `test_node_serialization.cpp`
A `FakeNodeDataResolver` returns a couple of fabricated `Recipe`/`Item` by name.
- Round-trip each node kind: build → `Serialize` → `Node::Deserialize` (with the
  fake) → `Serialize` again, assert the two JSON strings are equal.
- Factory dispatch: each `node_kind` string yields the correct `GetKind()`.
- Unknown kind throws `std::domain_error`.
- Recipe/item resolution: a `CraftNode` deserialized with the fake resolver
  binds the expected `Recipe*`; an unknown name resolves to `nullptr` without
  crashing.

---

## Part B — `rate_solver.cpp` (839 lines): decompose the monster `Solve`

### Problem
`RateSolver::Solve` is effectively the entire file (lines 31–839) — one function
with inline lambdas (`create_variable`, `compute_reduced_matrix`). 72.6% covered.
**No new DI is needed**: the only editor coupling is type-only
(`ax::NodeEditor::PinKind`/`FlowDirection` enums on `Pin`/`Link`), and the one
runtime editor value is already injected as `error_flow_duration`.

### B1. Phase decomposition
Introduce a file-local `SolverContext` struct holding the working state
(propagation queues, the strong/weak constrained pin sets, the variable list,
pin↔variable maps, the equation matrix + constants). Break `Solve` into named
member functions, called in order by a thin `Solve`:

1. `ResetErrorsAndFlow(nodes, links)` — clear per-pin errors and link flow.
2. `SeedFromConstraint(constraint_pin, value)` — first link / queue seed.
3. `CollectInvolvedPins()` — BFS gathering impacted + multi-pin sets.
4. `BuildVariables()` — `create_variable`; strong-constrained first, then rest.
5. `BuildEquations()` — assemble `coefficients · x = constants`.
6. `SolveLinearSystem()` — `compute_reduced_matrix` + back-substitution.
7. `ApplyResults()` — write rates back to pins / set link flow.

The public `Solve` signature is **unchanged**; behavior is preserved (verbatim
moves of each block into a method, shared locals promoted to context fields).

### B2. Tests — extend `test_rate_solver.cpp`
Add graph topologies to push coverage past 72% and pin down behavior before/after
the move: craft chains, mixed merger+splitter, locked-pin constraints, an
overflow/weak-constraint case, and a rejected-update (returns false, sets
`error_time`) case. Phase-level unit tests only where a phase is cleanly callable
on a fabricated context; otherwise rely on end-to-end `Solve` assertions.

---

## Sequencing & mechanics
1. **Part A first** (node), then **Part B** (rate_solver) — Part B's tests build
   graphs out of nodes, so a stable node layer first reduces churn.
2. Within each part: move/extract verbatim, build from repo ROOT
   (`cmake -S . -B build`), run `ctest -C Release` after each file/phase.
3. Pure refactor — no behavior, signature (except the added resolver param), or
   rendering change.

## Out of scope
- Splitting `node.hpp` into per-family headers (kept unified by decision).
- Any algorithm change in `Solve`.
- The known `domain → app/utils.hpp` violation (tracked separately).
- Touching `json.cpp` (cohesive library, not a target).

## Risks & mitigations
- **Resolver threading misses a `Data::` site** → grep `Data::` in the new node
  TUs must return empty after the move; compile errors surface unthreaded ctors.
- **Behavior drift in node deserialization** → the round-trip JSON-equality tests
  plus a manual app load/save guard the move.
- **`Solve` regression** (delicate algorithm) → extend tests *first* (capture
  current behavior), then decompose with the suite green at every step; each
  phase is a verbatim block move, not a rewrite.
- **GroupNode recursion** → the resolver must be forwarded at node.cpp:320; a
  group round-trip test covers it.
