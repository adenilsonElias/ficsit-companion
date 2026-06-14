# Vehicle Route Auto-Balance (Phase 2) — Progress

Design: `docs/superpowers/specs/2026-06-13-vehicle-route-auto-balance-design.md`.
Built TDD, no git commits (user preference); this file is the running log.

## Status: complete (automated)

Full suite green: **97 test cases / 440 assertions** (was 80/415 at Phase 2 start).
Desktop app target builds. GUI is unchanged — auto-balance rides the existing
interactive link-creation path (`production_app.cpp` calls `CreateLink(..., true)`),
so drawing a plug↔plug link now balances the route.

## What was built

1. **`VehicleRoute::GroupPoolByItem`** (`domain/vehicle_route.{hpp,cpp}`)
   — groups a pool's cargo pins by item into `PoolItemGroup{ item, supply, demand }`.
   Permissive (both sides required), excludes the fuel inlet and null-item pins.
   Tests in `tests/test_vehicle_station_node.cpp` (`[route]`): partition, fuel
   exclusion, null skip, one-sided omission, multi-item.

2. **Solver pool support** (`domain/rate_solver.cpp`)
   - BFS sub-case for `Logistics`: an *active cargo pin* (Load input / Unload
     output, never fuel) pulls its `(pool, item)` group in — supply+demand pins
     become relevant and their belt far-ends are propagated. Each (pool, item)
     expanded once (pool id = smallest member pointer).
   - One auxiliary **`T`** variable per active group (allocated after all pin
     variables; maps to no pin). Balance equations:
     `sum(unlocked supply) − T = −sum(locked supply)` and
     `T − sum(unlocked demand) = sum(locked demand)`.
   - §6 free-variable ratio: unlocked pins on a side keep their prior share of
     `(T − sum_locked)` — mirrors merger/splitter ratio with single_pin → T.
     (Carrying `sum_locked` in the constant was essential; dropping it produced
     an inconsistent equation that the rollback couldn't remove → infinite loop.)
   - **No-solution guard:** scan for an inconsistent row (all-zero coefficients,
     non-zero constant) right after the first reduction. An over-constrained
     route can be contradictory with `equations == variables`, which the existing
     "extra rows" check skips; without this the free-variable loop spun forever.
   - Tests in `tests/test_vehicle_route_solve.cpp` (`[vehicle_route]`): both
     directions, supply ratio, locked remainder, demand ratio, multi-item
     independence, permissive one-sided, machine→loader→unloader→sink multi-hop,
     over-constrained rejection.

3. **Re-solve triggers** (`domain/graph_model.cpp`)
   - `CreateLink` route branch: after wiring, `ResolveRoutePool` solves seeded
     from a pool member's non-zero active cargo pin; a rejected solve deletes the
     link (like belt links).
   - `DeleteLink` route branch: re-settles **both** resulting sub-pools (cutting
     a link can split the pool).
   - `IsActiveCargoPin` promoted to `domain/graph_item_resolve.{hpp,cpp}` (shared
     by solver and graph_model).
   - Tests in `tests/test_graph_model.cpp` (`[graph_model][vehicle_route]`):
     create balances, create rejects over-constrained, delete re-settles survivor.

## Follow-up fix — cargo item propagation on connect

Bug report: connecting a Load station (item set) to an empty Unload station did
not pass the item type across. Root cause: the route-link create path only did
`route_links` bookkeeping + rate solve and returned early — it never carried the
cargo item across, and `GroupPoolByItem` (which keys off existing items) produced
no group for the untyped unloader, so neither item nor rate propagated.

Fix: `VehicleRoute::PropagateCargoItems(pool)` fills empty cargo slots — each
loaded item onto an empty output of every unloader, each unloaded item onto an
empty cargo input of every loader (never overwrites; skips the fuel inlet).
`GraphModel::CreateLink` runs it (pool-wide) before the balance solve.
Tests in `tests/test_graph_model.cpp`: item passes supply→demand and demand→supply.

Related case — also fixed: belt-feeding a station *after* the route is connected.
`GraphModel::CreateLink`'s belt path now, after assigning the station's cargo
item, runs a `sync_route` step on any Truck/Train station in a 2+ member pool:
`PropagateCargoItems` then `ResolveRoutePool`, carrying the item across and
re-balancing rates. Best-effort (a rejected re-solve leaves rates; the belt link
stays). Test: `tests/test_graph_model.cpp` "feeding a loader belt syncs an
existing route".

## Follow-up fix — mark all cargo belts; flow only on connected belts

Bug report: connecting truck stations by vehicle marked only ONE output belt of
the unload station; both belts should show the item. User decision: mark both
belts with the type, but route flow should land only on belts actually connected
downstream (an idle belt is a type hint at rate 0).

Fix, two parts:
1. `VehicleRoute::PropagateCargoItems` now `fill_uniform`s a station's active
   side — if it ends up carrying a single distinct item, every empty belt on
   that side is marked with it (a second distinct item keeps its own belt).
2. `RateSolver` only treats a route belt as a balance participant when it is
   live: has a belt link, is locked, already carries a rate, or is the pin being
   edited. Idle, unconnected belts are skipped, so flow is never split onto a
   belt nothing is attached to (permissive group needs a live pin per side).

Tests: `test_vehicle_station_node.cpp` (single-item marks both belts; multi-item
keeps one belt each), `test_graph_model.cpp` ("marks both belts, flows only when
connected"). Pre-existing route solver/graph tests updated to mark the belts that
should carry flow as live (connected / in-use), matching the new semantics.

## Pre-commit code review fixes

- **Restore-mode mutation (main):** route-link item propagation and the belt-sync
  re-solve were ungated, so `CreateLink(..., trigger_update=false)` — used by
  session load and group paste — mutated restored state (filled cargo belts) and
  could re-solve pasted rates. Both are now gated on `trigger_update`; bulk
  restore only records the route link. Test: "leaves restored state untouched in
  restore mode" (`test_graph_model.cpp`).
- **Dedup:** extracted `SyncRoutePool(pool)` (= `PropagateCargoItems` + re-solve)
  shared by the route-create and belt-sync paths; `ResolveRoutePool` now takes a
  pre-computed pool (one `FindPool` per call site instead of two).
- **Pointer ordering:** the per-pool id in `rate_solver.cpp` uses
  `std::less<const void*>` instead of raw `operator<` on unrelated pointers.

Full suite green afterwards: 103 cases / 462 assertions; desktop app builds.

## Not done / out of scope (per design)

- Vehicle count / fuel burn / trip throughput (unchanged from Phase 1).
- GUI visual confirmation (headless here; the rendering path is unchanged).
