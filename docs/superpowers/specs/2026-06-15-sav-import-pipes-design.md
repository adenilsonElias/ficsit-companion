# Design: Pipe & Fluid-Container Import from `.sav`

**Date:** 2026-06-15
**Status:** Approved (pending spec review)
**Scope:** Extend the `.sav` importer so fluid (pipe) connections between
machines are reconstructed in the production graph, and fluid containers
(Pipe Storage Tank / Industrial Fluid Buffer) are placed as nodes.

---

## 1. Motivation

The `.sav` importer currently reconstructs conveyor-belt topology but
deliberately skips pipes (`wrapper.js` classifies `Build_Pipeline*` as `"pipe"`
and `continue`s; `Build_PipelineJunction*` is incorrectly mapped to a belt
`"merger"`). As a result, fluid producers (refineries, water/oil pumps,
generators that consume water/fuel) and fluid consumers land in the imported
graph completely unconnected, and fluid storage is missing entirely.

This work wires fluid machines together and places fluid containers, with **no**
pressure / head / flow-rate-limit simulation — the goal is to "mount the
connections with machines," matching how belts are imported.

---

## 2. Key insight: pipe networks, not chain-walking

Belts are reconstructed by walking `mConnectedComponent` chains hop-by-hop
through belt segments. Pipes do **not** need this:

- Every pipe connector component (`FGPipeConnectionComponent` on
  segments/junctions/pumps/tanks, and `FGPipeConnectionFactory` on machines)
  carries an `mPipeNetworkID` (int).
- `FGPipeNetwork` actors map each network id → the fluid it carries
  (`mFluidDescriptor`, e.g. `Desc_Water`, `Desc_HeavyOilResidue`) and list its
  member components (`mFluidIntegrantScriptInterfaces`).

Therefore the importer **groups machine fluid ports by `mPipeNetworkID`**.
Consequences, all verified against `example_save.sav`:

- **Pumps** (`Build_PipelinePump`) carry the *same* network id on both
  connectors (`Connection0=2, Connection1=2`). Grouping by network id absorbs
  them automatically — a pump is never a chain terminus / "dead wall."
- **Junctions** (`Build_PipelineJunction_T` 3-way, `_Cross` 4-way) have all
  connectors on one network id. They vanish into the network and are **not**
  modeled as belt mergers/splitters.
- **Pipe segments, supports, flow indicators, and pipe floor-holes** are pure
  plumbing and are not emitted as nodes.

Pipe networks are genuinely **many-to-many**. Examples from `example_save.sav`:

| net | fluid | producers | consumers |
|----:|-------|----------:|----------:|
| 2 | Water | 3 water pumps | ~14 coal generators |
| 7 | Heavy Oil Residue | 4 refineries | 1 refinery |
| 19 | Heavy Oil Residue | 2 | 4 |
| 20 | Heavy Oil Residue | 8 | 1 |
| 13 | Liquid Fuel | 1 refinery | 2 fuel generators + 3 tanks |

Because the node graph permits at most one `Link` per `Pin`, a producer cannot
fan its single output pin out to many consumers directly. The shared bus is
therefore represented by a **synthetic manifold node** (see §5).

---

## 3. Connector direction & fluid resolution

- Connector name → direction hint:
  - `PipeInput*` → `in` (consumer side)
  - `PipeOutput*` → `out` (producer side)
  - `ConnectionAny*` or bare `FGPipeConnectionFactory` → `any` (ambiguous)
- Ambiguous ports are common: a water pump's port is just
  `FGPipeConnectionFactory` (it produces), a coal generator's is the same name
  (it consumes), and tanks use `ConnectionAny0/1` (bidirectional).
- Direction is resolved authoritatively on the C++ side by **matching the
  network's fluid item against the node's recipe pins**: if the node has an
  output pin carrying the fluid it is a producer; if it has an input pin
  carrying the fluid it is a consumer. Extractors (water/oil pump) are
  producers. The wrapper's `dir` hint is a fallback only.
- Fluid name resolution reuses the wrapper's existing `itemDisplayName` plus
  `ITEM_DISPLAY_OVERRIDES` (e.g. `Desc_LiquidOil`→"Crude Oil",
  `Desc_LiquidFuel`→"Fuel", `Desc_Water`→"Water"); `Desc_HeavyOilResidue`
  resolves via CamelCase splitting to "Heavy Oil Residue".

Feasibility confirmed: generators import as `CraftNode`s with `Power (...)`
recipes that expose the fluid as an input pin (`Power (Coal)` ins = Coal +
Water; `Power (Fuel)` ins = Fuel), so the manifold has a pin to wire into.

---

## 4. Wrapper (`tools/sav_import/wrapper.js`, `wrapper_core.js`) changes

1. **Reclassify pipe plumbing.** Remove the `Build_PipelineJunction*`→`merger`
   mapping. Classify `Build_Pipeline*`, `Build_PipelineJunction*`,
   `Build_PipelinePump*`, `Build_PipelineSupport*`, `Build_PipelineFlowIndicator*`,
   and `Build_FoundationPassthrough_Pipe*` as plumbing — not emitted as
   buildings.
2. **New fluid-container building kinds:**
   - `Build_PipeStorageTank*` → `"fluid_buffer"`
   - `Build_IndustrialTank*` → `"industrial_fluid_buffer"`
3. **Build the network → fluid map.** Scan `FGPipeNetwork` actors for
   `mPipeNetworkID` + `mFluidDescriptor` → `{ id, fluid }`.
4. **Collect machine pipe endpoints.** For each emitted building, scan its
   `components` for pipe connectors (`FGPipeConnectionFactory` /
   `FGPipeConnectionComponent`), reading each connector's `mPipeNetworkID` and a
   `dir` hint from the connector-name suffix.
5. **Emit a new top-level `pipe_networks` array:**
   ```jsonc
   "pipe_networks": [
     { "id": 2, "fluid": "Water",
       "endpoints": [ { "building": "<actor id>", "dir": "any" }, ... ] }
   ]
   ```
   `building` is the emitted building's id; a building appears once per
   connector it owns on that network. Networks whose fluid does not resolve are
   still emitted (the C++ side warns and skips wiring).

The shared connector-scan / property helpers go in `wrapper_core.js` for unit
testing, mirroring the existing belt helpers.

---

## 5. C++ (`include/infra/sav_import.hpp`, `src/infra/sav_import.cpp`) changes

### 5.1 Data model
- `ParseResult` gains:
  ```cpp
  struct PipeEndpoint { std::string building; std::string dir; };
  struct PipeNetwork  { int id = -1; std::string fluid;
                        std::vector<PipeEndpoint> endpoints; };
  std::vector<PipeNetwork> pipe_networks;
  ```
- `BuildingKind` gains `FluidBuffer`, `IndustrialFluidBuffer`; `ParseKind`
  maps the two new strings.
- `ParseWrapperJson` parses the `pipe_networks` array.

### 5.2 New logistics kinds
Append to `LogisticsNode::Kind` (appending preserves saved kind indices):
```cpp
PipeJunction = 5,
FluidBuffer = 6,
IndustrialFluidBuffer = 7,
```
Add `GetDisplayName` cases: "Pipe Junction", "Fluid Buffer", "Industrial Fluid
Buffer". No icon/texture work needed (logistics nodes render by text name).

### 5.3 Node construction
- `FluidBuffer` → `LogisticsNode(Kind::FluidBuffer, 1 in, 1 out)`.
- `IndustrialFluidBuffer` → `LogisticsNode(Kind::IndustrialFluidBuffer, 2, 2)`.

### 5.4 Manifold wiring (new pass in `BuildGraph`, after belt wiring)
For each `PipeNetwork` whose `fluid` resolves to a known `Item`:

1. **Resolve endpoints.** For each endpoint building present in the graph,
   classify as producer / consumer:
   - Node has an **output** pin whose item == fluid → producer (record that pin).
   - Node has an **input** pin whose item == fluid → consumer (record that pin).
   - `ExtractorNode` whose resource == fluid → producer (its single output).
   - Fluid buffer (`LogisticsNode` with no recipe) → consumer on the network it
     draws from; also a producer when it **bridges two distinct networks** (its
     output feeds the second network). A buffer with both connectors on one
     network is a consumer-only tap (its output left unconnected) to avoid
     cycles.
   - Otherwise fall back to the wrapper `dir` hint.
2. **Skip** networks that resolve to 0 producers or 0 consumers (nothing to
   connect); buffer nodes are still placed.
3. **1 producer + 1 consumer** → wire the producer's fluid output pin directly
   to the consumer's fluid input pin (no manifold node).
4. **Otherwise (≥1 producer and ≥1 consumer, >2 endpoints total)** → create one
   `PipeJunction` `LogisticsNode` with `#producers` inputs and `#consumers`
   outputs. Link each producer's fluid output → a distinct junction input, and
   each junction output → a distinct consumer's fluid input. Stamp the fluid
   `Item` on every junction pin and on the matched machine pins. Position the
   junction node near its endpoints' centroid; `ApplyCompactLayout` re-lays it
   out in Compact mode.
5. Respect one-link-per-pin: if a chosen machine pin already holds a link
   (e.g. the same pin was claimed by a belt), skip that endpoint and count it.

Rate propagation: the existing logistics passes in `BuildGraph`
(`IsLogistics()` sum-inputs / distribute-outputs, plus the demand-conservation
pass) already handle the new `PipeJunction` and fluid-buffer nodes, so imported
fluid rates settle the same way belt logistics do. No new rate code is needed
beyond ensuring the new kinds report `IsLogistics() == true` (inherited).

### 5.5 Warnings / diagnostics
Aggregate, matching the belt style:
- count of pipe networks wired, manifolds created, direct 1:1 links.
- networks skipped for unresolved fluid.
- networks skipped for 0 producers or 0 consumers.
- endpoints skipped because the target pin was already linked.

---

## 6. Out of scope

- No pressure, head-lift, or flow-rate-limit modeling.
- Pumps are not nodes; junctions are not nodes.
- Hypertubes (`FGPipeConnectionComponentHyper` / `Build_PipeHyper*`) are not
  fluid logistics and are ignored.

---

## 7. Testing

- **`tools/sav_import/test/wrapper_core.test.js`** — unit-test the new
  connector-scan / network-grouping / direction-hint helpers on synthetic
  fixtures (segment, pump, junction, tank, refinery in/out ports).
- **`tests/test_sav_import.cpp`** — `ParseWrapperJson` parses `pipe_networks`;
  `BuildGraph` produces: a direct link for a 1:1 network; a `PipeJunction`
  manifold with the right pin counts for an N:M network; a `FluidBuffer` node
  placed and wired; correct fluid item stamped; an endpoint whose pin is
  already linked is skipped (no collision); a network with unresolved fluid is
  skipped with a warning.
- **End-to-end:** run the wrapper on `example_save.sav` and confirm the Water,
  Crude Oil, Heavy Oil Residue, and Liquid Fuel networks wire as expected and
  no regression in belt counts.

---

## 8. Known simplifications

- A fluid buffer bridging two networks assigns its in/out networks without flow
  analysis (acceptable — no head simulation).
- Manifold flow is distributed evenly by the existing logistics propagation,
  then pulled toward downstream demand by the demand-conservation pass, exactly
  as belt logistics; this is best-effort, not a fluid solver.
