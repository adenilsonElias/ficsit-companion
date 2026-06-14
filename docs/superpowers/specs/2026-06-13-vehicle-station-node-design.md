# Vehicle (Truck/Train) Station Node — Design

**Date:** 2026-06-13
**Status:** Approved (brainstorm)
**Scope:** Manually-placed truck station node in the production modeler. Built
generically so train stations (and later tractors/drones) reuse it; **trucks
implemented and tested first**.

## 1. Goal

Give the manually-placed truck station node a vehicle-aware model:

- A real belt layout matching the in-game building.
- A **Load / Unload** toggle.
- A distinct, colored **vehicle plug** that connects stations into shared
  vehicle routes (the "many-to-many" cargo pool).

Today a `LogisticsNode` with `logistics_kind == TruckStation` exists but only
has generic belt-style pins (3 in / 2 out) and no vehicle semantics. Logistics
nodes are pure passthroughs in the solver: each pin is an independent variable,
so inputs and outputs are not linked.

## 2. Delivery in two phases

- **Phase 1** — node model, plug, multi-link links, Load/Unload toggle,
  serialization + save migration, right-click menu entry, and a **route-summary
  panel**. Rates are entered/locked manually (matches today's passthrough
  Logistics semantics — the solver is untouched).
- **Phase 2** — extend `RateSolver` with pool discovery + per-`(pool, item)`
  balance equations for true demand-driven auto-propagation across the route.

The phase boundary exists to de-risk the solver change without blocking a
usable feature.

## 3. Node model

A dedicated `VehicleStationNode : public LogisticsNode`, distinguished
internally by `logistics_kind` (`TruckStation` / `TrainStation`).

- `GetKind()` stays `Node::Kind::Logistics` so the existing solver switch and
  all code that branches on node kind keep working unchanged.
- `Node::Deserialize` routes `logistics_kind ∈ {TruckStation, TrainStation}`
  to this subclass.
- **No new `Node::Kind`** is added (avoids save-format churn). The subclass
  split keys off `logistics_kind`.

> Rationale for subclass vs. enriching `LogisticsNode` directly: vehicle-only
> data (mode, plug) does not apply to Storage / Industrial Storage /
> Dimensional Depot. A subclass keeps those kinds clean while sharing the
> existing serialization/base behavior.

### Belt layout (fixed, matches the real building)

- **Left (inputs):** 2 cargo-in pins + 1 fuel-in pin.
- **Right (outputs):** 2 cargo-out pins.
- Belt pins are normal typed `Pin`s — fully reused by existing
  link/belt/solver code.
- The **fuel** pin is an ordinary belt input with a user-set rate (a free sink;
  never auto-derived from vehicle count or trip distance — the planner cannot
  know those).

### Mode

New member `enum class Mode { Load, Unload }`.

- The toggle changes **only the vehicle plug's direction** and which belts are
  semantically "active". All belt pins always render.
- **Load:** cargo-in belts feed the station; vehicle plug is an **Output**
  (source into the route pool). Cargo-out belts are visible but carry 0 /
  unconnected.
- **Unload:** vehicle plug is an **Input** (sink from the pool); cargo-out
  belts deliver. Cargo-in belts are visible but carry 0 / unconnected. (Fuel-in
  stays usable in both modes.)

## 4. Vehicle plug & routes

- One **vehicle plug** per station, stored as a dedicated
  `std::unique_ptr<Pin> plug` member (outside `ins` / `outs`), rendered in a
  distinct color/shape vs. belt pins.
- Direction follows `Mode` (Load → Output, Unload → Input).
- The plug is **untyped** (`item == nullptr`) and conceptually carries the
  aggregate cargo bundle (all cargo items the station handles).
- **Multi-link:** the plug holds a `std::vector<Link*>` instead of the single
  `Pin::link`. Link creation special-cases plugs to skip the "one link per pin"
  rejection.
- **Link validity:** plug links are only valid **plug ↔ plug** and only between
  a Load-side plug (output) and an Unload-side plug (input). Belt↔plug and
  same-direction plug links are rejected. Belt link rules are unchanged.
- A **pool** is a connected component over plug links. Discovered on demand:
  for the summary panel in Phase 1, and for the solver in Phase 2.

### Worked example

Four stations A, B, C, D; one truck loads Rotor from A, B, C and unloads at D.
The truck is **not** a node — the plug links *are* the route. A, B, C are in
Load mode (plug = output); D is in Unload mode (plug = input). Because plugs are
multi-link, D's single input plug receives three links.

```
[Rotor src]──belt──►┌ Station A (Load) ┐●╮
                    └ cargo-in    plug ┘ │
                                         │
[Rotor src]──belt──►┌ Station B (Load) ┐●┼──►●┌ Station D (Unload) ┐──belt──►[Rotor consumer]
                    └ cargo-in    plug ┘ │    └ plug      cargo-out ┘
                                         │
[Rotor src]──belt──►┌ Station C (Load) ┐●╯
                    └ cargo-in    plug ┘
```

- Pool = connected component `{A, B, C, D}`.
- Rotor balance: `supply = A.in + B.in + C.in`, `demand = D.out`.
- Vehicles / truck count never appear; the model tracks cargo flow only, not
  how many trucks or trips realize it.

### Route identity

**Implicit.** There is no truck or route entity. A route is simply the set of
plug links forming a connected pool. Two unrelated routes are two separate
connected components. (No route labels, no truck nodes.)

## 5. Distribution semantics (Phase 2 target)

When a pool has multiple unloaders, cargo is distributed **by downstream
demand**: each unloader pulls what its belt-output side actually needs
(propagated from downstream machines / locks). The solver matches loader supply
to the sum of unloader demands, using exact `FractionalNumber` arithmetic, and
errors when supply ≠ demand. In Phase 1 this relationship is shown in the
summary panel and entered manually.

## 6. Serialization & migration

- `VehicleStationNode::Serialize` extends `LogisticsNode`'s output with:
  - `mode`
  - the serialized `plug` (its id + its link endpoints)
  - fuel-pin designation (which input index is the fuel pin)
- Plug links serialize like belt links but reference plug ids.
- **Old saves:** existing generic `TruckStation` logistics nodes load as plain
  logistics (no plug). Optional one-time migration gives them a plug + default
  `Load` mode. Acceptable because this is an upgrade fork.

## 7. UI

- Right-click "Add node" menu: "Truck Station" creates a `VehicleStationNode`
  (Truck). "Train Station" likewise, reusing the class.
- On the node header: a Load / Unload toggle (radio or switch).
- The vehicle plug is rendered in a distinct color/shape so it is obviously not
  a belt pin.
- **Route-summary panel:** selecting a station (or hovering its plug) shows,
  for its pool, per-item supply (sum of loader cargo-inputs) vs. demand (sum of
  unloader cargo-outputs), flagging imbalance. This is the Phase-1 stand-in for
  demand balancing and the read-off for "by downstream demand".

## 8. Testing (Catch2)

Phase 1:

- Node construction and pin layout per mode (2 cargo-in + 1 fuel-in, 2
  cargo-out, 1 plug).
- Toggle flips plug direction; belts remain present.
- Serialization round-trip, including plug, multi-link plug links, mode, and
  fuel-pin designation.
- Link-validation rules: plug↔plug only, Load→Unload only, multi-link allowed
  on plugs, belt rules unchanged.
- Pool / connected-component discovery over plug links.
- Route-summary supply/demand math (incl. the A,B,C→D example).

Phase 2:

- Solver pool balance: per-`(pool, item)` equation links loader cargo-inputs to
  unloader cargo-outputs.
- Multi-unloader distribution by downstream demand.
- Imbalance → error, exact-fraction correctness.

## 9. Train reuse

`VehicleStationNode` is vehicle-type-agnostic. `TrainStation` reuses the same
plug / toggle / pool logic; only the display name, menu entry, and (if desired)
belt counts differ — no separate code path.

## 10. Out of scope

- Modeling vehicle count, fuel consumption rate, trip time, or throughput
  capacity.
- Explicit truck/vehicle nodes or named routes.
- Integration with the `.sav` import auto-wiring path and the standalone
  Vehicle Map tool (these remain independent; unifying them is future work).
