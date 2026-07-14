# Snapshot: Real-Efficiency Rates & Belt-Throughput Overlay

**Date:** 2026-06-30
**Tool affected:** Factory Snapshot (read-only `.sav` view)
**Status:** IMPLEMENTED 2026-07-11 (see PROGRESS.md). All components landed and
tested (232 C++ / 23 JS). Two deviations from this design, made during
implementation:

1. **Productivity field:** real 1.2 saves have no `mCurrentProductivity`.
   `readProductivity` uses the measurement-window pair
   `mLastProductivityMeasurementProduceDuration / mLastProductivityMeasurementDuration`
   (current-window pair as fallback, else 1.0).
2. **Rate re-application:** instead of `ApplyRates()` (re-seed + forward
   `PropagateRates` only), the efficiency toggle **re-imports** from the retained
   wrapper JSON via `FactorySnapshotApp::RebuildSnapshot()`, reusing the whole
   importer pipeline (forward propagation + the demand-pullback consistency pass
   + vehicle-route balancing + flow report). This avoids dropping the pullback
   polish that `ApplyRates` alone would have lost, keeping both toggle states
   internally consistent. Efficiency is folded in at the producer seed, gated by
   `SavImport::BuildOptions::apply_efficiency`. `PropagateRates` (Component 3) is
   still extracted and used by the importer.

## Goal

Two linked visualization features for the Factory Snapshot:

- **A — Real-efficiency rates (toggle, default ON):** read each machine's
  productivity from the save and fold it into its input/output rates, so the
  snapshot shows *actual* throughput (input-starved / output-blocked machines
  running below their clock), not just the nominal clock × base rate.
- **B — Belt-throughput overlay (toggle, default OFF):** optionally draw the
  items/min carried by each link (belt/pipe) on the canvas.

Overclock and somersloop are already applied to machine rates today; efficiency
is the missing factor. The two features share one rate-propagation step so belt
throughput stays consistent with the efficiency-adjusted machine outputs.

## Background (current behavior)

- The importer reads each manufacturer's clock (`mCurrentPotential`) and
  somersloops, and sets `CraftNode`/`ExtractorNode` pin rates to
  `base × clock × somersloop` via `UpdateRate` (`sav_import.cpp:882,913`).
- After building the graph, `sav_import.cpp` runs an inline ~100-line
  forward-propagation loop (lines ~1718–1830) that fills belt / organizer /
  storage / sink rates to a fixed point.
- The snapshot **does not display power**, so scaling rates by efficiency
  affects nothing else in this view.
- `model.flow` (the Resources-tab report) is computed from machine pin rates by
  `BuildResourceFlowReport(nodes)`.
- The snapshot model is otherwise treated as read-only; the existing
  hide-logistics feature never mutates it.

## Behavior summary

- With **Apply efficiency** ON (default), every producer's rate is
  `clock × efficiency × somersloop`; belts, organizers, storages, sinks, and the
  Resources-table totals all reflect the adjusted values. OFF restores nominal
  `clock × somersloop` rates.
- With **Show throughput** ON, each drawn link shows the items/min it carries
  (the source pin's `current_rate`), positioned at the link midpoint. OFF =
  today's behavior.
- Toggling either option recomputes/redraws immediately and persists in the
  session.
- If the save lacks a readable productivity value for a machine, that machine's
  efficiency defaults to `1.0` (no change), so the feature degrades safely.

## Architecture

The snapshot gains an explicit **rate pipeline** that can be re-run when the
efficiency toggle changes:

```
producer rate = clock × (apply_efficiency ? efficiency : 1) × somersloop   [UpdateRate]
              → PropagateRates(nodes)   [belts/organizers/storage/sink]
              → BuildResourceFlowReport(nodes)   [Resources table]
```

Decision: **extract the existing inline propagation loop** from `sav_import.cpp`
into a reusable pure domain function so both the importer and the snapshot app
call one implementation (DRY, unit-testable, and it shrinks the 2000-line
importer). This is a targeted improvement to code we are modifying, not an
unrelated refactor.

Rejected alternatives:
- **Duplicate the propagation logic in the app** — violates DRY, two code paths
  to keep in sync.
- **Store two rate sets (nominal + effective) per pin and swap on toggle** —
  invasive to the display and flow code; recomputing is simpler and reuses the
  one pipeline.
- **Apply efficiency only at display time** — belts/flow would not stay
  consistent without re-deriving them anyway.

Note on the read-only invariant: toggling efficiency recomputes *derived* data
(pin `current_rate` values and `model.flow`); graph **topology** (nodes/links)
is never mutated. This is an intentional, scoped relaxation for derived rates.

## Component 1 — Productivity extraction (wrapper)

In `tools/sav_import/wrapper.js` (manufacturer + generator path near line 559,
and the extractor path), read the machine's productivity and emit it on the
building entry:

```js
// 0..1; defaults to 1.0 when the save has no readable productivity.
efficiency: readProductivity(props),
```

`readProductivity` lives in `wrapper_core.js` (testable, single source of
truth). The Satisfactory save stores productivity for buildables either as a
direct value or as a measured produce-duration / total-duration pair; the
function tries the known property names and returns a clamped `[0,1]` ratio,
falling back to `1.0`:

```js
// Pseudocode — exact property names verified against a real save during impl.
function readProductivity(props) {
    const direct = readNumberProp(props, "mCurrentProductivity", NaN);
    if (Number.isFinite(direct)) return clamp01(direct);
    const produce = readNumberProp(props, "mCurrentProductivityMeasurementProduceDuration", NaN);
    const total   = readNumberProp(props, "mCurrentProductivityMeasurementDuration", NaN);
    if (Number.isFinite(produce) && Number.isFinite(total) && total > 0)
        return clamp01(produce / total);
    return 1.0;
}
```

**Verification step (implementation):** run the wrapper on the user's save and
inspect one manufacturer's parsed properties to confirm the real field name(s);
adjust `readProductivity` accordingly. Until confirmed, the `1.0` fallback keeps
behavior identical to today.

## Component 2 — Importer plumbing

- `SavImport::Building` gains `double efficiency = 1.0;` parsed from the
  wrapper JSON `efficiency` key (mirrors how `clock` is parsed).
- The snapshot build records, per producer node, its nominal `clock` and
  `efficiency` so the rate can be recomputed later without re-reading JSON.
  Stored in the snapshot model (avoids changing the shared `CraftNode` class):

  ```cpp
  // In FactorySnapshotModel:
  struct ProducerRate { ax::NodeEditor::NodeId node; FractionalNumber clock; FractionalNumber efficiency; };
  std::vector<ProducerRate> producer_rates;
  ```

  Populated for each `CraftNode`/`ExtractorNode` as it is created in the
  importer/builder.

## Component 3 — Extracted propagation + rate pipeline

New files: `domain/snapshot/rate_propagation.hpp` / `.cpp`.

```cpp
// Forward-propagate rates from producer outputs through the wired graph to a
// fixed point: merger out = sum(ins); splitter/logistics outs = in / connected
// outs; link copies upstream output rate onto downstream input pin, except
// CraftNode/Extractor pins (recipe-fixed). Pure; no UI. Mutates pin
// current_rate only.
void PropagateRates(const std::vector<std::unique_ptr<Node>>& nodes);
```

The body is moved verbatim from `sav_import.cpp` (lines ~1718–1830). The
importer calls `PropagateRates(out.nodes)` in place of the inline loop, so import
behavior is unchanged.

Snapshot-app rate pipeline (new `FactorySnapshotApp::ApplyRates()`):

```cpp
void FactorySnapshotApp::ApplyRates()
{
    for (const auto& pr : model.producer_rates) {
        Node* n = FindNode(pr.node);            // craft/extractor
        const FractionalNumber rate = session.apply_efficiency
            ? pr.clock * pr.efficiency : pr.clock;
        static_cast<PoweredNode*>(n)->UpdateRate(rate);
    }
    PropagateRates(model.nodes);
    model.flow = BuildResourceFlowReport(model.nodes);
}
```

Called once after import and whenever `apply_efficiency` changes. (`UpdateRate`
already folds somersloop into the pins, so it is not re-multiplied here.)

## Component 4 — Throughput overlay

In `RenderGraphCanvas`, while rendering nodes, capture each submitted pin's
screen-rect center keyed by pin id:

```cpp
std::unordered_map<uintptr_t, ImVec2> pin_centers; // filled during the node pass
// after DrawPinIcon/marker for a pin p:
//   pin_centers[p.id.Get()] = midpoint(ImGui::GetItemRectMin(), GetItemRectMax());
```

After `ax::NodeEditor::End()`, when `session.show_throughput` is on, iterate
`visible_edges` and draw the source pin's rate at the midpoint between the two
endpoints' centers using the window draw list:

```cpp
for (const CachedEdge& ce : visible_edges) {
    auto s = pin_centers.find(ce.edge.start_id.Get());
    auto e = pin_centers.find(ce.edge.end_id.Get());
    if (s == pin_centers.end() || e == pin_centers.end()) continue;
    const ImVec2 mid = midpoint(s->second, e->second);
    const FractionalNumber rate = RateOfPin(ce.edge.start_id); // source output pin rate
    DrawThroughputLabel(mid, rate.GetStringFloat() + "/min");
}
```

`RateOfPin` looks up the output pin's `current_rate` (build a pin-id → Pin* map
once per frame, or reuse an existing lookup). The label uses a small background
pill for legibility and scales with the existing `collapsed_font_scale` so it
stays readable when zoomed out. Pin centers exist even for collapsed nodes
(their pins submit 1px markers), so the overlay works at all zooms.

## Component 5 — Persistence & UI

`FactorySnapshotSession` gains:

```cpp
bool apply_efficiency = true;    // fold save productivity into rates
bool show_throughput  = false;   // draw items/min on links
```

`Serialize`/`Deserialize` handle both (additive keys; missing → defaults).

Options tab (`RenderOptionsPanel`), in the existing "Declutter / view" area:

- `Checkbox("Apply real machine efficiency (from save)", &apply_efficiency)` —
  on change: `SaveSession(); ApplyRates();`.
- `Checkbox("Show belt throughput", &show_throughput)` — on change:
  `SaveSession();` (overlay reads the flag each frame; no recompute needed).
- "Reset to defaults" also resets these two fields and calls `ApplyRates()`.

## Testing

- `test_rate_propagation.cpp` (new, Catch2): moved-out `PropagateRates` —
  merger sums inputs; splitter divides among connected outs; logistics divides;
  multi-hop chain converges; CraftNode/Extractor pins are not overwritten.
  (Port assertions from existing import tests that currently cover the inline
  loop, so coverage is preserved.)
- `test_factory_snapshot_session.cpp`: round-trip + default-on/off of
  `apply_efficiency` and `show_throughput`.
- `wrapper_core.test.js`: `readProductivity` — direct value, duration-pair
  ratio, clamping to [0,1], and `1.0` fallback when absent.
- `test_sav_import.cpp` (or builder test): a manufacturer with `efficiency < 1`
  yields producer pin rates scaled by it when efficiency is applied; importer
  output unchanged when `efficiency == 1`.
- Existing import tests must still pass after the propagation extraction
  (behavior-preserving refactor).

The throughput overlay rendering itself (ImGui draw calls) is verified manually.

## Out of scope

- No power display changes (snapshot shows no power).
- No change to the Production or Vehicle Map tools.
- No persistence-format migration (additive keys; old sessions use defaults).
- Stored inventory counts (the other interpretation of "item counts") are not
  included; only belt/pipe throughput.

## Manual verification

After implementation, re-import the user's save and confirm:
- Machines that are input-starved/output-blocked in-game show reduced rates with
  **Apply efficiency** on, and nominal rates with it off.
- The Resources table totals shift between the two modes accordingly.
- **Show throughput** draws correct items/min on belts at all zoom levels, and
  matches the producing machine's (efficiency-adjusted) output.
