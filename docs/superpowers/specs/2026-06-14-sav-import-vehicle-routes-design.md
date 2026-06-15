# `.sav` Import — Vehicle Routes via Plugs + Fuel Pin Color

Date: 2026-06-14

## Problem

The modeler models truck/train stations as `VehicleStationNode`: a Load/Unload
`mode`, a vehicle **plug** pin held outside `ins`/`outs`, and `route_links`.
Drawing a plug↔plug link forms a *route pool* (`VehicleRoute`) that auto-balances
cargo across the connected component (completed Phase 2 work).

The `.sav` import does **not** use this mechanism:

- It creates a plain `LogisticsNode` for stations (`sav_import.cpp:947`) — no plug,
  no mode, no `route_links`.
- Its `connect_vehicle_routes` option wires loader **cargo-output** → unloader
  **cargo-input** as ordinary belt-style `Link`s — the old mechanism, not the
  plug/pool one.

This is also a latent inconsistency: the node deserialization factory
(`node_base.cpp:117`) creates `VehicleStationNode` for these kinds, so an imported
session that is saved and reloaded silently upgrades the importer's plain
`LogisticsNode` stations into `VehicleStationNode`s.

Goal: make the import produce `VehicleStationNode`s with the correct mode and wire
vehicle routes as plug↔plug route links (the modeler's mechanism), auto-balancing
each pool — and give the fuel inlet pin a distinct color in the renderer.

## Decisions

- **Switch to plug routes** (drop the old cargo-pin route links).
- **Auto-balance pools** after wiring, best-effort.
- **Fuel pin color**: reuse the existing plug orange `ImColor(255, 170, 0)`.

## Design

### 1. Station nodes become `VehicleStationNode`

The importer creates a `VehicleStationNode` instead of `LogisticsNode` for
`TruckStation` / `TrainStation` kinds, with:

- **mode** from the parsed `Building::is_unloader` (`is_unloader → Mode::Unload`,
  else `Mode::Load`),
- cargo pin counts kept from the importer's existing observed-port logic (so no
  dead-end pins), plus the fuel inlet last and the vehicle plug for the mode.

`VehicleStationNode`'s only public constructor hardcodes 2 cargo-in + 1 fuel + 2
cargo-out, which would reintroduce dead-end pins. So add a constructor overload:

```cpp
VehicleStationNode(NodeId id, LogisticsNode::Kind kind, Mode mode,
                   size_t cargo_in, size_t cargo_out,
                   const std::function<unsigned long long int()>& id_generator);
```

It allocates `cargo_in` cargo inputs + 1 fuel input (last) + `cargo_out` outputs,
sets `mode`, and builds the plug via `PlugDirectionFor(mode)`. This also fixes the
save/reload upgrade inconsistency described above.

### 2. Fuel pin distinct color (renderer)

In the input-pin draw loop (`production_app.cpp:1944`), when `IsFuelPin(p.get())`
is true, draw the pin circle in fuel orange `ImColor(255, 170, 0)` (the plug color
from `:202`) instead of white. Both the empty (`AddCircle`) and connected
(`AddCircleFilled`) cases get the color. Purely cosmetic.

### 3. Wire routes as plug↔plug links (replaces old cargo-pin wiring)

The current `connect_vehicle_routes` block (`sav_import.cpp:1411-1562`) is
replaced. For each parsed `vehicle_route`:

- connect every Load station's plug (Output) to every Unload station's plug
  (Input) — complete bipartite within the route, so all its stations land in one
  `VehicleRoute` pool;
- record each new `Link` in **both** stations' `route_links`; leave `Pin::link`
  null on the plugs (matching the modeler);
- mode comes from the node, not from belt inspection;
- dedup identical loader/unloader pairs across routes (a `created` set keyed by the
  node pointer pair, as the old code did).

### 4. Auto-balance each pool

After belt wiring and the existing best-effort rate-propagation pass have given
loaders their cargo-input rates, run — per distinct pool (`VehicleRoute::FindPool`,
visiting each station once) — `PropagateCargoItems` + the route solve.

Promote the `SyncRoutePool` / `ResolveRoutePool` helpers out of `graph_model.cpp`'s
anonymous namespace into `VehicleRoute` (domain) so the importer and the modeler
share one implementation. Signatures keep the `nodes`/`links` vectors and the
`error_time` / `error_flow_duration` parameters; the headless importer passes a
throwaway float and `0` duration.

Best-effort: a rejected or over-constrained solve (and any `std::runtime_error`
from `RateSolver`) is caught, leaves links and rates intact, and is reported as an
import warning. The summary warning line keeps the existing
`[vehicle routes] N station link(s) created` shape.

### 5. Testing

Extend `test_sav_import.cpp`:

- stations import as `VehicleStationNode` with the correct `mode` (load vs unload
  from `is_unloader`);
- a route produces plug `route_links` on the stations, and **no** cargo-pin route
  `Link`s;
- a loader→unloader route balances cargo rate across the pool (loader cargo-input
  rate appears on the unloader cargo-output after the pool solve);
- the fuel inlet is excluded from route wiring;
- a unit test for the new `VehicleStationNode` constructor (pin counts, fuel pin
  last, plug direction per mode).

Existing route/graph tests that referenced the promoted helpers update to the new
`VehicleRoute::` location.

## Out of scope

- Train-specific topology (no trains in the sample save; class names only).
- Vehicle count / fuel burn / trip throughput.
- Reconstructing the true driven sub-path between route stations.
