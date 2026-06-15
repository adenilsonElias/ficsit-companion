# `.sav` Import — Vehicle Routes via Plugs + Fuel Pin Color — Progress

Spec: `docs/superpowers/specs/2026-06-14-sav-import-vehicle-routes-design.md`
Plan: `docs/superpowers/plans/2026-06-14-sav-import-vehicle-routes.md`
Built TDD via subagent-driven development; no git commits (user preference) — this is the running log.

## Status: complete

Full suite green: **109 test cases** (was 103 at start). Desktop app + `fc-tests` build clean.
Final holistic review: ready to merge (no Critical/Important findings).

## What was built

1. **`VehicleStationNode` importer constructor** (`domain/node.hpp`, `domain/vehicle_station_node.cpp`)
   — `(id, kind, mode, cargo_in, cargo_out, id_generator)`: allocates `cargo_in` cargo
   inputs + 1 fuel inlet (last) + `cargo_out` outputs, sets mode, builds the plug via
   `PlugDirectionFor(mode)`. Lets the importer keep its observed-port pin counts.
   Test: `test_vehicle_station_node.cpp` "importer ctor honors mode and cargo counts".

2. **Promoted `ResolveRoutePool` / `SyncRoutePool`** out of `graph_model.cpp`'s anon
   namespace into the `VehicleRoute` domain module (`domain/vehicle_route.{hpp,cpp}`),
   so the modeler and importer share one pool-balance implementation. `graph_model.cpp`
   call sites now use `VehicleRoute::` (CreateLink, mode/item change, DeleteLink split).

3. **Importer builds stations as `VehicleStationNode`** (`infra/sav_import.cpp`)
   — branch in the station-construction switch: Truck/Train → `VehicleStationNode`
   (mode from `Building::is_unloader`), other logistics kinds stay `LogisticsNode`.
   Also fixes the latent save/reload "silent upgrade" inconsistency (the deserialization
   factory already built `VehicleStationNode`). Removed the now-dead `has_fuel_input_pin`
   write + stale comments for the station path.
   Test: "imports stations as VehicleStationNode with mode".

4. **Routes wired as plug↔plug `route_links`** (`infra/sav_import.cpp`)
   — replaced the old cargo-pin route wiring (and its helpers: `is_station_node`,
   `StationWire`, `resolve_item_upstream`, `free_out_pin`, `free_in_pin`). For each route,
   complete-bipartite Load-plug(Output) → Unload-plug(Input), deduped across all routes by
   node-pointer pair, recorded in both stations' `route_links` (Pin::link left null).
   Warning gated on `> 0`. Header comment on `connect_vehicle_routes` updated.
   Tests: "wires vehicle routes as plug route links", "dedups a shared station pair across
   routes", "creates no route link for a loader-only route".

5. **Auto-balance each route pool** (`infra/sav_import.cpp`)
   — after the rate-propagation fixed-point pass, per distinct pool (`VehicleRoute::FindPool`,
   `visited` dedup) run `VehicleRoute::SyncRoutePool`. Best-effort: a rejected solve or
   `std::runtime_error` is caught (logged to stderr), counted, and reported as a warning;
   links/rates left intact. The downstream link-consistency check is unaffected (plug rates
   stay 0==0). Test: "balances cargo rate across an imported route pool".

6. **Fuel inlet pin color** (`app/production_app.cpp`)
   — in the inputs draw loop, the station fuel inlet (`IsFuelPin`) draws in the plug orange
   `ImColor(255, 170, 0)`; cargo pins stay white. Both empty/connected paths. Outputs loop
   untouched. Cosmetic; visual confirmation left to the user.

## Known cosmetic notes (non-blocking)
- `production_app.cpp` fuel-pin `?:` mixes int/float `ImColor` overloads (same color).
- `ResolveRoutePool` seeds from the first non-zero active cargo pin (shared modeler
  semantics); multi-item pools settle around that seed. Zero-rate routes stay at 0 (by design).

## Not done / out of scope (per spec)
- Train-specific topology (no trains in the sample save).
- Vehicle count / fuel burn / trip throughput.
- Reconstructing the true driven sub-path between route stations.
