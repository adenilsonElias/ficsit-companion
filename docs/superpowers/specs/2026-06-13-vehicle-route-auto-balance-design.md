# Vehicle Route Auto-Balance (Phase 2) — Design

**Date:** 2026-06-13
**Status:** Approved (brainstorm)
**Depends on:** Phase 1 — `docs/superpowers/specs/2026-06-13-vehicle-station-node-design.md`
(implemented; see `docs/superpowers/plans/2026-06-13-vehicle-station-node-PROGRESS.md`)

## 1. Goal & scope

Make the rate solver propagate cargo through a vehicle route pool automatically.
Editing any belt rate (or a machine up/downstream) settles the whole
`upstream → loader → pool → unloader → downstream` chain in **one** solve, using
exact `FractionalNumber` arithmetic.

Per item, per pool, the invariant is:

```
sum(loader cargo-input rates for item I) = sum(unloader cargo-output rates for item I)
```

Two semantic decisions (from brainstorming):

- **Permissive:** balance an item only when the pool has BOTH at least one loader
  input and at least one unloader output carrying it. An item present on only one
  side gets no equation (never forced to 0), so routes can be built incrementally.
  The Phase 1 hover summary still flags the imbalance.
- **Ratio-preserving:** when a side is underdetermined (e.g. 3 loaders supplying a
  total fixed by downstream demand), each unlocked pin keeps its prior share of
  the total; locked pins keep their value. This matches the existing
  merger/splitter free-variable behaviour.

Out of scope: modelling vehicle count / fuel burn / trip throughput (unchanged
from Phase 1); the fuel pin remains a free user-set sink.

## 2. Background: how the solver works today

`RateSolver::Solve` (`domain/rate_solver.cpp`) is a Gaussian-elimination solver
over per-pin variables:

1. **BFS** from `constraint_pin` over `pin->link` collects `relevant_pins`.
   Craft/Group/GameSplitter: any pin update pulls in all the node's pins.
   Merger/CustomSplitter: a "single" pin plus deferred "multi" pins. Sink /
   Logistics / Extractor: trigger no neighbours.
2. **Variables:** one per pin, except Craft/Group/GameSplitter share one variable
   across the node. Logistics pins get one variable each.
3. **Equations:** the user constraint; one equality per link
   (`start·ratio − end·ratio = 0`); one balance per merger/splitter
   (`sum(ins) − sum(outs) = 0`, locked pins folded into the constant).
4. **Solve:** reduce; resolve free variables (merger/splitter keep ratio relative
   to their single pin; everything else keeps current value); back-substitute.
5. Reject on negative rates or no solution; otherwise assign rates to pins/nodes.

Vehicle plugs hold no `pin->link` and are invisible to this machinery (Phase 1).
Phase 2 adds pool awareness without disturbing the existing flow.

## 3. Pool item grouping (`domain/vehicle_route`)

A new pure helper, unit-testable without the solver:

```cpp
struct PoolItemGroup
{
    const Item* item = nullptr;
    std::vector<Pin*> supply;  // Load-mode stations' cargo inputs carrying item (excl. fuel pin)
    std::vector<Pin*> demand;  // Unload-mode stations' cargo outputs carrying item
};

/// @brief Group a pool's cargo pins by item. Returns only groups with BOTH
/// sides non-empty (permissive). Fuel inlets and inactive-side belts are excluded.
std::vector<PoolItemGroup> GroupPoolByItem(const std::vector<VehicleStationNode*>& pool);
```

Rules:
- A **Load**-mode station contributes its cargo **input** pins (all `ins` except
  the fuel inlet, identified via `IsFuelPin`) to `supply`.
- An **Unload**-mode station contributes its cargo **output** pins (`outs`) to
  `demand`.
- Pins with a null item are skipped (not yet typed → inactive).
- A station's inactive-side belts (a loader's outputs, an unloader's inputs) are
  ignored.

`FindPool` (Phase 1) supplies the pool; `GroupPoolByItem` is the new piece.

## 4. Solver BFS extension (pool traversal)

The plug stays structural (no scalar variable, no `pin->link`). To connect the
two sides of a route into one solve, extend the BFS: when it makes an **active
cargo pin** of a vehicle station relevant (a Load-mode cargo input or an
Unload-mode cargo output, never the fuel pin), look up the pin's pool, compute
its `PoolItemGroup` for that pin's item, and:

- mark every `supply` and `demand` pin in the group relevant, and
- push each such pin's belt-link far-end onto the propagation queue (so the
  upstream feeding a loader and the downstream pulling an unloader are drawn into
  the same solve).

This is a new sub-case in the BFS `switch` for `Node::Kind::Logistics`, gated on
the node being a Truck/Train station and the pin being on its active side. Each
pool/item group is expanded once (track visited groups by `(pool, item)`).

## 5. Balance equations + per-group "total" variable

For each active `(pool, item)` group reached by the solve, introduce **one
auxiliary variable `T`** = the route total for that item. `T` is **not** a pin —
just a solver variable allocated after all pin variables. Add two equations,
folding locked pins into the constant exactly as the merger/splitter balance does:

```
sum(unlocked supply pins) − T = −sum(locked supply pins)
T − sum(unlocked demand pins) =  sum(locked demand pins)
```

This makes the group behave precisely like **a merger (supply → T) feeding a
splitter (T → demand)**, so the existing balance/ratio mathematics applies
unchanged in spirit. Because `T` maps to no pin:

- the negative-rate check and the final rate-assignment loops (which iterate
  `associated_variable_index` keyed by `Pin*`) skip it naturally;
- `reversed_variable_map[T_index]` is left null and never dereferenced as a pin.

Allocation order: create all pin variables first (so existing pivot ordering is
unchanged), then one `T` per active group.

## 6. Free-variable resolution (ratio preservation)

When a group side is underdetermined, its pins must keep their prior ratio. With
`T` standing in for the merger/splitter "single pin," the generalisation is
direct:

- **Supply pins** are resolved like a merger's multi-inputs relative to `T`:
  each unlocked supply pin keeps `pin_old / sum(supply_old) · T`; locked pins keep
  their value.
- **Demand pins** are resolved like a splitter's multi-outputs relative to `T`,
  by the same formula.

This reuses the existing `old_sum_not_constrained` ratio logic (rate_solver.cpp
free-variable block), parameterised so the "single pin" variable is the group's
`T` and the "multi pins" are the group's supply (resp. demand) pins. When a side
has a single pin, the ratio step degenerates to `pin = T`, i.e. a plain equality.

## 7. Triggering re-solves

Phase 1 left plug links inert. Phase 2 must settle rates when the route topology
changes:

- **Create plug link:** after wiring two plugs, run a solve seeded from an
  affected belt pin so the new route balances (mirroring how belt-link creation
  triggers a solve). If it fails (over-constrained), reject and delete the link,
  same as belt-link creation.
- **Delete plug link / delete station:** re-solve from a remaining pool member so
  the now-smaller pool re-settles. A pool that drops to a single side becomes
  permissive (no balance), leaving rates as-is.
- **Load/Unload mode toggle** (`production_app`): after `SetMode` drops the
  station's route links, re-solve is moot (links gone); no special action beyond
  the existing link deletion.

## 8. Edge cases

- **Permissive:** items lacking one side produce no equation (no forced 0).
- **Over-constrained** (locked supply total ≠ locked demand total): no solution →
  reject the edit, flash red, revert — identical to today's belt behaviour.
- **Fuel pins / inactive belts:** never part of any group.
- **Multi-item pools:** each item is an independent group with its own `T`.
- **Repeated item on one station** (two cargo inputs both carrying item I): both
  pins join the group's supply; ratio resolution splits between them.
- **Negative results:** caught by the existing negative-rate check; the edit is
  rejected.

## 9. Testing

Headless (GraphModel + RateSolver, no GUI):

- `GroupPoolByItem` unit tests: supply/demand partition, fuel-pin exclusion,
  null-item skip, permissive filtering (one-sided item omitted), multi-item.
- Solver:
  - single loader ↔ single unloader, **both** directions (set the loader input →
    unloader output follows; set the unloader output → loader input follows);
  - 2 loaders → 1 unloader: total fixed by demand, loaders keep prior ratio;
    one loader locked → the other absorbs the remainder;
  - 1 loader → 2 unloaders: outputs driven by downstream demand, loader = sum;
  - multi-item pool: Rotor and Screws balance independently in one pool;
  - permissive: loader carries Rotor, unloader carries Screws → no forced 0,
    both keep their set values;
  - multi-hop: machine → loader cargo-in, unloader cargo-out → sink/machine, all
    settled in one solve;
  - over-constrained (locked mismatch) → solve rejected.

## 10. Files

- `domain/vehicle_route.{hpp,cpp}` — add `GroupPoolByItem`.
- `domain/rate_solver.cpp` — BFS pool traversal, `T` variables, balance + ratio
  equations, processed-group tracking.
- `domain/graph_model.cpp` — trigger a solve on plug-link create/delete.
- `tests/test_vehicle_station_node.cpp` (or a new `tests/test_vehicle_route_solve.cpp`)
  and `tests/test_rate_solver.cpp` — the cases in §9.

## 11. Risks

- `rate_solver.cpp` is intricate; the free-variable ratio generalisation (§6) is
  the highest-risk change. Mitigation: build incrementally behind tests — first
  balance with single pins per side (§5), then multi-pin ratio (§6) — and keep the
  pool grouping logic in `vehicle_route` where it can be tested in isolation.
- The `T` variable changes `num_variables`; audit every loop that assumes a
  variable index maps to a pin (negative check, rate assignment, link-flow reset)
  to ensure `T` indices are skipped.
