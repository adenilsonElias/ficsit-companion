# SAV Import Mixed Item Investigation Progress

Date: 2026-06-12

## Context

We are continuing the `sav-import-mixed-item-merger-investigation` work.

The current reported issue:

- UI debug id `9433` was observed as a splitter.
- The UI displayed the splitter as `Plastic`.
- Walking backward through the connections in the modeler reached a `Miner Mk1` that should be a `Coal` miner.

The saved session does not serialize runtime/editor node ids, so `9433` cannot be mapped directly after reload. Runtime ids depend on the id generator state during import/session activity.

## Evidence Gathered

Parsed:

- `build/ficsit-companion/Release/last_session.fcs`

Saved graph summary:

- `674` nodes
- `791` links
- No serialized node `id` field
- Link endpoints use saved node array indices and pin indices

I searched for the same topology pattern instead of the runtime id and found a matching failure mode:

```text
#516 GameSplitter item=Iron Ore
  <- #514 GameSplitter item=Coal
    <- #518 Merger item=Coal
      <- #519 CustomSplitter item=Coal
        <- #513 Extractor resource=Coal item=Coal

#516 GameSplitter item=Iron Ore
  -> #490 Craft recipe=Steel Ingot
  -> #489 Craft recipe=Steel Ingot
```

Interpretation:

- Splitter `#516` is labeled `Iron Ore`.
- Its upstream source is extractor `#513`, which is `Coal`.
- It feeds `Steel Ingot` foundries.
- `Steel Ingot` has multiple inputs, including `Iron Ore` and `Coal`.
- The importer appears to be using downstream recipe input demand or physical pin fallback to label an upstream pass-through chain, even when the real upstream source is known as `Coal`.

This is the same class of problem the user reported: a splitter label is being inferred from the wrong side of the chain.

## Likely Root Cause

In `ficsit-companion/src/infra/sav_import.cpp`, item inference currently:

- Seeds `belt_item` from known producers like extractors and simple craft outputs.
- Propagates item evidence through plain organizers/logistics.
- Deliberately excludes smart/programmable splitters from uniform propagation because their outputs may carry different filtered items.
- Later walks upstream from craft input pins and stamps the ingredient onto upstream organizers/logistics.

That smart-splitter wall is correct for not forcing all smart splitter outputs to one item, but it also means a known upstream item may fail to cross a specific smart splitter output branch. Then the downstream craft fallback can repaint the normal splitter as a recipe ingredient based on the connected craft input.

Concrete saved graph symptom:

- Coal from extractor reaches smart/custom splitter.
- The branch continues to a normal splitter.
- The normal splitter feeds a `Steel Ingot` foundry.
- The normal splitter ends up labeled `Iron Ore`, not `Coal`.

## Regression Test Added

File changed:

- `ficsit-companion/tests/test_sav_import.cpp`

Added helpers:

- `SmartSplitter(...)`
- `Miner(...)`

Added test:

```text
BuildGraph keeps upstream resource item through smart splitter before multi-input craft
```

Scenario:

```text
Coal miner -> smart splitter -> normal splitter -> Steel Ingot foundry
```

Expected behavior:

- The normal splitter remains typed as `Coal`.
- The belt into the foundry is routed to the foundry's `Coal` input pin.
- The importer must not repaint that splitter as `Iron Ore` just because the physical save-port or unresolved fallback hit the first foundry input slot.

Status: **FIXED (2026-06-12).** The test was run and confirmed to fail with
`"Iron Ore" == "Coal"` (Steel Ingot Foundry's `ins[0]` is Iron Ore, `ins[1]`
is Coal — the unresolved Coal belt landed on the Iron Ore pin by physical rank,
then the upstream-stamping pass repainted the plain splitter as Iron Ore). The
importer was then fixed (see below) and the test now passes.

## Previous Completed Work In This Investigation

Already implemented and verified before the pause:

- Conveyor lift floor holes are handled in the JS save wrapper via `Build_FoundationPassthrough_Lift`.
- Added `wrapper_core.js` and JS tests for floor-hole belt-chain following.
- Added `show_debug_ids` setting.
- Modeler can display node/pin ids and hovered link ids when `Show debug IDs` is enabled.
- C++ mixed-item detection was changed to use upstream/incoming item evidence rather than downstream multi-input craft demand only.
- Release app was rebuilt after previous changes.
- Full Catch2 suite previously passed `67/67`.

## Next Steps When Resuming

1. Run the focused test and confirm it fails for the expected reason:

```powershell
ctest --test-dir build -C Release --output-on-failure -R sav_import
```

If filtering by Catch2 test name is easier, run the built `fc-tests.exe` with the specific test case filter.

2. Fix `SavImport::BuildGraph` item inference.

Likely direction:

- Keep smart/programmable splitters as item-separation walls for uniform `ChangeItem`.
- But allow a known upstream item to propagate through an individual smart splitter branch to the specific outgoing belt.
- Prefer known upstream producer item evidence over downstream multi-input craft demand.
- Avoid stamping a normal organizer from downstream if its upstream chain already resolves to a conflicting concrete item.

3. Rerun:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

4. Rebuild Release app if tests pass:

```powershell
cmake --build build --config Release
```

## Fix Applied (2026-06-12)

Root cause: in `SavImport::BuildGraph`, the forward `belt_item` propagation
(`inherit_from_organizer`) treated smart/programmable (custom) splitters as a
hard wall — it returned immediately for them. A known upstream item therefore
could not cross the splitter, the downstream belt stayed unresolved, the
rank-based wiring dropped it on the foundry's first physical input pin (Iron
Ore), and the upstream craft-input stamping pass walked back and relabeled the
plain splitter as Iron Ore.

Fix: a splitter's outputs are always a subset of its inputs, so a custom
splitter fed by a **single distinct, fully-resolved** input item can only emit
that item on every output. `inherit_from_organizer` now propagates that one
input item onto the custom splitter's **output belts only** (never output→input
or output→sibling), and bails if any input belt is still unresolved or if the
inputs disagree — so a genuinely item-filtering splitter (fed by two different
items) stays a propagation wall.

- `ficsit-companion/src/infra/sav_import.cpp` — custom-splitter branch added in
  `inherit_from_organizer`.
- `ficsit-companion/tests/test_sav_import.cpp` — regression test + `#include
  "domain/recipe.hpp"` (needed the full `Item` type for `->name`).

Verification:

- Focused test passes (9 assertions).
- Full Catch2 suite: **68/68 passed**, no regressions.
- Release app rebuilt successfully (`ficsit-companion.exe`).

## Fix Applied — miner mislabeled "Plastic" (2026-06-12)

Reported: a miner showed resource **Plastic** (UI node id 7513); Plastic is not
mineable — it should have been **Coal**.

Evidence (from `build/ficsit-companion/Release/last_session.fcs`): node index
119 was the only extractor with a non-ore resource (`Plastic`, `extractor_kind`
0 → unset, so the save recorded neither its kind nor its resource). Its output
feeds node 646, a **CustomSplitter labeled "Plastic"** (a smart splitter the
player configured with a Plastic output filter), whose `out1` branch goes to
node 648 `GameSplitter(Coal)` → Black Powder (Coal + Sulfur). So Coal is what
physically flows; the "Plastic" is just the splitter's filter label.

Root cause: `ResolveExtractorResources` → `ResolveDownstreamItem` returned the
**first item on the downstream pin**. `CustomSplitterNode`'s constructor stamps
its filter item onto its input pin (`organizer_nodes.cpp:146`), so the miner's
downstream pin carried "Plastic", which the resolver returned verbatim — even
though a miner can only produce a raw resource.

Fix (`ficsit-companion/src/infra/sav_import.cpp`):
- Added `IsExtractableResource(const Item*)` — the closed set of raw resources an
  extractor can produce (solid miner ores + SAM + Water + Crude Oil + Nitrogen
  Gas).
- Replaced `ResolveDownstreamItem` with `ResolveExtractorResourceDownstream`: a
  breadth-first downstream walk that accepts **only** raw extractable resources.
  This rejects filter/stale labels (Plastic) and makes it safe to traverse
  smart/programmable splitters (a splitter's outputs are a subset of its single
  raw input), so the walk continues past node 646 to the Coal evidence.
- `test_sav_import.cpp` — added regression test "BuildGraph resolves a miner past
  a smart splitter's filter label to the raw resource".

Verification:
- New test failed pre-fix with `"Plastic" == "Coal"`, passes post-fix.
- Full Catch2 suite: **69/69 passed**, no regressions.
- Release app rebuilt successfully.

Note: this corrects the **importer**; the already-imported `last_session.fcs`
still shows the old label. Re-import the save (or re-run the `.sav` import) for
node 7513 to be recomputed as Coal.

## Tool: mapping runtime node/link ids to saved `.fcs` indices (2026-06-12)

The user reports bugs by the **runtime** ids shown in the app (debug ids on), but
the `.fcs` does not serialize ids. They are reproducible though: on load,
`GraphModel::next_id` starts at 1 and `GetNextId()` is handed out in array order
— per node: one id for the node, then one per pin (Craft/Extractor pins rebuilt
from recipe: `ins` then `outs`; organizers/logistics/sink: serialized `ins` then
`outs` count), then one id per link in `links` order.

So `runtime_id = sim_id + B` for a constant offset `B` (the import started
`next_id` at `B+1`; for the user's `debug.fcs`, **B = 7066**, verified against two
independently-located storages). A Python replay of this assignment maps any
runtime id the user cites to a saved node/link index. Pin counts for Craft nodes
come from `assets/satisfactory.json` (recipe ins+outs).

## Truck station 9388 — belt on "input vs output" (2026-06-12)

Mapped: runtime node 9388 → saved node **#580** =
`Build_TruckStation_C_2147342468` (matched by position; node.pos = world ×0.4).
Link 10478 → **belt-689**: `#580 station.out0 → #588 GameSplitter(Plastic)`.

Ran the wrapper on `teste-1.2-beta_120626-002605.sav` with a temporary connector
dump. The station's connectors:
- `Output0`/`Output1` → `ConveyorAny0`  (this is belt-689 / link 10478)
- `Input0`/`Input1`/`Input2` → `ConveyorAny1`  (belt-697, from splitter 2146630086)

So **in the save file, link 10478's belt is on the station's OUTPUT connectors**
— the importer reads it faithfully. Open question: is the user mis-reading the
in-game side, or do truck-station `Output*`/`Input*` connector roles need to be
interpreted differently (note the anomalous `index:-1` from 0-based `Output0`/
`Input0` names and the duplicate connectors)? Needs the user to confirm the
station's load/unload mode. (Debug dump reverted; wrapper.js unchanged.)

## Storage 7515(Coal) → 7822(Plastic) — the deep cross-wire (2026-06-12)

Mapped: saved nodes **#120 (Coal storage) → #198 (Plastic storage)**. The chain
`#646 CustomSplit(Coal) → #120 → #198 → … → #202 → #480 GameSplit(Plastic)` is
**linear and single-sourced from a Coal smart splitter**, so it must be Coal.
But `#480` is a GameSplitter whose single Coal input fans out — impossibly — to
**Coal, Iron Ore, Plastic, Steel Pipe, Wire, and Circuit Board** splitters/crafts
(`#479/#481/#482 Plastic`, `#226/#223 Coal`, `#225/#194 Iron Ore`, `#463 Steel
Pipe`, `#462 Wire`, `#632 Circuit Board`, feeding crafts `#219-221`, `#191-193`,
`#459-461`, …). A single splitter cannot carry six items.

This is the original investigation's core **mixed mega-component**.

### ROOT CAUSE FOUND (2026-06-12): truck/train station item bleed (NOT a walker cross-wire)

Ran the wrapper on the save and dumped the **raw** belts around this region
(mapping saved nodes → buildings by position). The raw topology is **faithful and
correct** — there is no fabricated edge and no Plastic anywhere physically:

- `#480`(`2147069685`) ← storage `#202`; → truck station `#426`(`2146672668`) + splitter `#479`.
- `#479`(`2147081044`) ← `#480`; → truck station `2147222635` + splitter `#481`.
- `#481`(`2147004430`) ← `#479`; → truck station `2147385111` + splitter `2146975662`.

It's a **Coal spine**: a row of splitters each tapping Coal off to a truck
station (loading Coal onto trucks) and passing the rest down the line. Every
belt is Coal.

The Plastic is an **item-inference artifact**, injected through truck stations
that are fed one item on a belt but deliver a *different* item via vehicle. E.g.
station `2147222635` is `mIsInLoadMode=false` (unloader) fed Coal on its **input**
belt by `#479`, yet its **output** belt carries what trucks bring (Plastic). The
importer propagates items straight across a logistics node's input→output
(`BuildGraph` step 8, `out->item = in_item`, and the vehicle-route step), but a
vehicle **decouples** the two sides of a station. So a vehicle-delivered Plastic
bleeds across the physically-Coal belt spine, and that wrong label floods backward
over the storage chain → the visible `7515(Coal)→7822(Plastic)` symptom.

**Refined (2026-06-12, after user feedback + belt-only reachability check):**
A blanket "item break" at stations is WRONG — a station legitimately combines a
belt input and a truck delivery of the SAME item into one output line, and that
must flow through. Evidence: a **belt-only** forward BFS from the Coal spine
reaches **0 manufacturers and 10 truck/train stations** — the spine never
physically touches a Plastic consumer. So the Plastic is injected by the
**station / vehicle-route layer** (`sav_import_connect_vehicle_routes=true`), not
a belt cross-wire and not craft-input stamping.

The real fix must be **item-aware in the vehicle-route / station item handling**:
a station's truck-delivered item must match what its route actually carries, and
a wrong item must not be attached to a Coal station and then flood the spine.
Same-item belt+truck combine must keep flowing through. Exact propagation path
that paints `#480/#479/#481` Plastic still needs an instrumented C++ repro to
pin down, but it is confined to the station/vehicle-route code, NOT `followBeltChain`.

### ACTUAL ROOT CAUSE (2026-06-12, user insight + verified): fuel belts mis-tagged as cargo

The user identified it: the Coal "spine" is a **vehicle-FUEL distribution line**.
Trucks need fuel; truck stations have a dedicated fuel inlet. The spine feeds Coal
into the stations' **fuel** slots; the stations' **cargo** is Plastic (arrives by
truck). The importer is wiring the Coal **fuel** belts as **cargo**, which dumps
Coal into the cargo item-space and produces the whole mixed mess.

Verified from the save:
- All 10 spine→station belts have `role: null` (NOT `"fuel"`) — treated as cargo.
- Station `2146672668`: `mIsInLoadMode=false` (unloader), two belts only — **Coal on
  `Input0`, Plastic on `Output0`**. For an unloader, cargo exits via the OUTPUT
  conveyor, so a belt on an INPUT can only be FUEL.
- `stationFuelConnectorSuffix` (wrapper.js:668) uses an "unpaired input = fuel"
  name heuristic → returns `Input2`, which has **no belt**. The real Coal fuel belt
  on `Input0` (a name-paired input) is never matched, so role stays null.
- `IsFuelItem` (utils.cpp:265) already includes "Coal", and the C++ fuel-pin
  routing fires when `belt.dst_role=="fuel"` and the item is a fuel. So **the fix is
  wrapper-only**: tag the belt `role:"fuel"`.

**How the save marks fuel (answered):** it doesn't. The connector components are
empty `FGFactoryConnectionComponent`s holding only `mConnectedComponent` (the belt
pointer). Which connector is the fuel inlet is defined by the TruckStation
**blueprint/class**, not the save, and the parser exposes one physical connector
under several alias names (`Input0/1/2`, `Output0/1`), so name-matching is
unreliable. The reliable signal is **load mode + direction**.

Connector survey across all 44 stations with belts (`mIsInLoadMode` + connected
connector names):
- Loaders (`load=absent`/default): cargo is an INPUT — 16 stations use just `Input1`.
- Unloaders (`load=false`): cargo is on the OUTPUT (`Output0/1`); the input belt is
  `Input0` = fuel. No unloader has cargo on a belt input (cargo arrives by truck).

**Fix plan (wrapper.js, confirmed mechanics):** a station has one cargo conveyor
whose direction follows load mode, plus an always-input fuel conveyor.
- Read `mIsInLoadMode` per station.
- **Unloader** (`false`): cargo = output belt; tag every belt landing on an INPUT
  connector `role:"fuel"`. (Solid — fixes the reported Coal-fuel-as-cargo bug.)
- **Loader** (`true`/absent): cargo = input belt. Distinguish the extra fuel input
  from the cargo input (fuel input observed as `Input0`, cargo as `Input1`) — or
  fall back to item-based (`role:"fuel"` only when the belt item is a known vehicle
  fuel). Lower priority; the reported bug is all unloaders.

C++ side needs no change: `IsFuelItem` includes Coal and the importer already
routes a `role:"fuel"` belt whose item is a fuel to the station's dedicated fuel
pin, isolating it from cargo. Resolves the whole mixed-item mega-component (the
Coal "spine" becomes a fuel network, cargo stays Plastic).

### BEST SIGNAL FOUND (2026-06-12): the station's FuelInventory tells the fuel item

The game DOES record fuel-vs-cargo, not on the connector but in the station's
**inventory components**. Each Truck/Train station has:
- `inventory` (FGInventoryComponent) — the **cargo** buffer; `mInventoryStacks`
  holds the cargo item(s).
- `FuelInventory` (FGInventoryComponent) — the **fuel** buffer; `mInventoryStacks`
  holds the fuel item.

Verified across the save: 21/46 stations have `FuelInventory` populated (all Coal),
cargo inventories hold the real cargo (OreIron, SteelPlate, SteelPipe, Wire, …).
E.g. `2146672668`: **cargo=OreIron, fuel=Coal** — distinct items, cleanly separable.

This cleanly solves even the hardest case (the user's): a loader fed **Coal as
cargo + Turbofuel as fuel** — item-only and direction-only fail (both are fuels;
both inputs), but `inventory=Coal` / `FuelInventory=Turbofuel` disambiguate the two
input belts by matching each belt's item to the right inventory.

**Final fix design:**
1. Wrapper: export per-station `fuel_item` (from `FuelInventory.mInventoryStacks`)
   and optionally `cargo_item` (from `inventory`).
2. C++ `BuildGraph`: when wiring a belt into a station input, if the belt's
   resolved item == the station's `fuel_item` (and ≠ cargo item) → route it to the
   dedicated fuel pin / mark fuel. Reuses existing fuel-pin machinery.
3. Fallbacks when `FuelInventory` is empty (25/46 stations, idle): (a) unloader →
   input belt is fuel (cargo exits via output); (b) belt item is a known vehicle
   fuel and differs from the cargo item → fuel. Same-item cargo+fuel stays
   ambiguous but harmless (no item mixing).

### IMPLEMENTED + VERIFIED (2026-06-13)

- Wrapper (`wrapper.js`): added `stationInventoryItem()`; truck/train station
  entries export `fuel_item` (FuelInventory), `cargo_item` (inventory), and
  `is_unloader` (`mIsInLoadMode === false`).
- C++ (`sav_import.hpp`/`.cpp`): `Building` gained those three fields, parsed in
  `ParseWrapperJson`. `BuildGraph` runs an early station-fuel classification on a
  mutable copy of the belts (rest of the function uses that copy): for each belt
  into a station input it traces the source item upstream (`trace_source_item`),
  then tags `dst_role="fuel"` when the item matches the station's `fuel_item` /
  the factory-wide fuel set (and ≠ cargo), or — when the source item is unknown —
  when the station is an unloader. Tagged belts flow through existing fuel-pin
  machinery.
- Tests: 2 new Catch2 cases (fuel belt → fuel pin; loader fuel/cargo split by
  inventory). Full suite **71/71**; wrapper JS tests 2/2.
- Real-save verification: **16 station-input belts now classified fuel**, incl.
  `belt-697`→station `2147342468` (UI id 9388, cargo Copper Sheet) and
  `belt-707`→`2147071783` (cargo **Plastic**) — the Coal fuel that was bleeding
  into cargo.

~~Known gap: the Coal spine traces to the bug-#1 miner whose resource is blank...~~
**Gap CLOSED (2026-06-13):** `BuildGraph` now resolves each blank-resource miner's
raw resource on the parsed belt graph (downstream BFS to the first raw extractable
ingredient, mirroring `ResolveExtractorResourceDownstream`) before fuel
classification, via `resolve_miner_resource` + a `resolved_miner_resource` map that
`trace_source_item` consults. Real-save check: 21/29 blank miners now resolve, and
the spine belts classify as fuel by **item-match** (item='Coal') rather than the
unloader fallback — so loader stations fed by the spine are caught too
(belt-697→station 9388, belt-699→Plastic cargo, …). 9 fuel belts via item-match,
8 via unloader fallback, 26 correctly left as cargo.

### Bug: Coal miner shown as Iron Ore — FIXED at source (2026-06-13)
After re-import, miner #119 (Coal) showed **Iron Ore** (it was Plastic before).
Root cause: the wrapper's `readExtractorResourceAndPurity` reads `mResourceClass`/
`mItemType`/`mPurity`, but this save's resource nodes (e.g. `BP_ResourceNode577`)
store the resource in **`mResourceClassOverride`** (an ObjectProperty →
`.../Desc_Coal_C`) and purity in **`mPurityOverride`**. So the miner came back with
an empty resource and fell to the downstream guess, which returns the *first* raw
input of a downstream multi-raw craft (Steel Ingot's `ins[0]` = Iron Ore) — wrong.
Fix (`wrapper.js`): read `mResourceClassOverride` / `mPurityOverride` as fallbacks.
Verified: **all 40 miners now resolve** (0 empty; node #119 = Coal, pure). This
removes reliance on the flawed downstream inference for these miners.

### Bug: Plastic merger labelled Copper Sheet (node 9387 area) — NOT YET FIXED
The chain `#577→#578→#587→#594→#581(TruckStation)` is labelled **Copper Sheet**,
but **every upstream producer is a Plastic refinery** (`#569`,`#570`, and `#576`
all trace to Plastic crafts) — there is NO Copper Sheet producer feeding it. The
Copper Sheet label originates **downstream**, at truck station **#581** whose cargo
`inventory` reads Copper Sheet (`ins`/`outs` pin items = Copper Sheet), and floods
**backward** up the Plastic belt-input chain, then out through the whole manifold.

This is the **cargo-side twin of the fuel bug**: a station's cargo/vehicle item
bleeds onto its belt-input chain and overrides the real upstream item. The belt
into the station carries Plastic (from the refineries); the station's Copper Sheet
arrives by truck (vehicle route) or is stale — the two are physically decoupled,
but the importer's item inference unifies them. Fix direction (analogous to the
fuel fix): a station's belt-**input** item must come from its upstream producer,
not from the station's cargo inventory / vehicle item; do not propagate the
station cargo item backward onto the feeding belt chain. Needs implementation +
a repro test. (Re-importing with the miner+fuel fixes first may shift these labels,
so re-check against a fresh import before building the cargo-side fix.)

### Bug: Plastic merger labelled Copper Sheet — FIXED (2026-06-13)
Re-confirmed after the miner fix (node id 9343 area). The manifold
`#577/#576/#587/#578/#573/#594` (+ stations `#581`,`#561`) is labelled Copper
Sheet but **every producer is a Plastic refinery**; it feeds **Circuit Board**
(`#571`,`#572`) and **Computer** (`#563`) assemblers, which genuinely need Copper
Sheet. Two compounding causes:
1. The Plastic recipe is **multi-output** (Plastic + Heavy Oil Residue), so the
   seed phase (single-output only) left the refinery belts untyped → the whole
   manifold was untyped going into step 6.
2. Step 6 (upstream craft-input stamping) **overrode** organizers with the craft's
   ingredient: the belt's real item had no matching pin on AI-Limiter/Circuit-
   Board-type consumers, so it landed on the Copper Sheet pin by rank, and the
   stamp repainted the whole Plastic chain Copper Sheet and flooded the manifold.

Fixes (`sav_import.cpp`):
- Seed `belt_item` for **multi-output** producers from the belt's ranked output
  port (`recipe->outs[rank]`). Verified: all 10 Plastic refineries expose exactly
  one belt output (port 0 = Plastic; Heavy Oil Residue is a fluid/pipe), so
  rank 0 → Plastic.
- Step 6 now only stamps **untyped** organizers/pins and never overrides a
  producer-resolved item (upstream supply wins over downstream demand); it also
  stops walking up through an organizer already typed to a different item.
- Tests: 3 new Catch2 cases (producer-resolved organizer kept vs downstream
  demand; refinery-fed merger stays Plastic; + the earlier fuel/cargo split).
  Full suite **73/73**.

### Bug: unloader with same fuel & cargo item shows fuel as cargo — FIXED (2026-06-13)
Station 7822 (an unloader) receives Coal **cargo by truck** (to make Steel) and
Coal **fuel by belt** (from node 8820); the importer showed both as cargo. The
fuel cascade matched `item == cargo_item` first and marked the belt cargo. But an
unloader's cargo arrives by vehicle and leaves via the OUTPUT conveyor, so an
INPUT belt is always the fuel inlet — even when fuel and cargo are the same item.
Fix (`sav_import.cpp`): for an unloader, an input belt is fuel whenever its item is
a known/factory fuel or unresolved, regardless of the cargo item; the item-vs-cargo
disambiguation now applies only to loaders (which genuinely take cargo on a belt
input). New Catch2 test; suite **74/74**. Real save has 2 such same-item unloaders
(`2147222635`, `2147427424`, both Coal).

### INCIDENT (2026-06-13): root CMakeLists.txt deleted and reconstructed
A cleanup command `rm -f import_dbg.json *.txt` was run from the repo root; `*.txt`
matched **`CMakeLists.txt`**, deleting it. It was tracked but had unstaged working
edits (Catch2/test wiring not in the committed version), so the exact file wasn't
recoverable from git. Reconstructed it as the committed root file plus
`include(cmake/catch2.cmake)` + `enable_testing()` under `NOT EMSCRIPTEN` (verified:
reconfigure succeeds, 71/71 tests build and pass, CTest discovers them). Lesson:
never glob-delete `*.txt` at the repo root.

### Truck station 9388 — RESOLVED as faithful
`mIsInLoadMode=false` (unloader) and belt-689 sits on its `Output*` connectors;
across all stations, load mode correlates cleanly with connector side (loaders →
`Input*` belts, unloaders → `Output*` belts). The import matches the save, so the
reported "input vs output" is not an importer bug for this station (its mode in
the save is unloader). Same underlying station-item-decoupling issue as above may
still make its *item* wrong, but the side is faithful.
