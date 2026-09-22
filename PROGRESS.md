# Progress: Multi-link pins in the Production Planner (IN PROGRESS)

Spec: `docs/superpowers/specs/2026-07-13-multi-link-pins-design.md`
Plan: `docs/superpowers/plans/2026-07-13-multi-link-pins.md`

Goal: a pin accepts N links; the pin's rate is the sum of its links' rates. Wiring a new
consumer onto an already-linked pin ramps production up to meet the summed demand, instead of
being rejected. Planner only — Factory Snapshot / Vehicle Map stay at one link per pin.

No git ops (per user preference). All code text in English (per user preference).

### Task 1 — model: `Pin::links` + `Link::current_rate` — DONE
- `Pin::link` (`Link*`) → `Pin::links` (`std::vector<Link*>`), plus `Pin::SoleLink()` which
  returns the link only when there is exactly one (nullptr for zero *or* fan-out, so callers
  that must handle fan-out are forced to iterate).
- `Link::current_rate` added (the per-edge rate). Not yet computed — Task 2 fills it.
- Migrated ~116 call sites across 13 source files + 5 test files.
- **Plan correction:** the plan claimed only `graph_test_helpers.hpp` would need editing. False —
  `test_pin`, `test_graph_model`, `test_node_serialization` and `test_sav_import` all reference
  `pin->link` directly (one as an lvalue). Renaming the field mechanically forces edits there.
  The safety net was refined: existing tests may be edited *only* to change field access; no
  assertion may be removed or weakened. Verified by diff.
- Suite: 232/232 green, unchanged from baseline. Behaviour is identical for single-link graphs.
### Task 2 — solver: one variable per link + per-pin balance — DONE
- Replaced the per-link `start == end` equality with a variable per link and a balance equation
  per pin: `sum(link vars of pin) - coef(pin) * var(pin) = 0`. On a single-link pin this reduces
  to the old equality, which is why every pre-existing test still passes untouched.
- **Bug the plan missed:** the seed reached a fanning pin but only ever expanded the *other* pins
  of its node, never that pin's own remaining links. In the single-link world that was fine (the
  one link was the way back). With fan-out the second consumer never entered `relevant_pins`, and
  `BuildLinearSystem` threw `invalid unordered_map key`. Fixed by expanding every link of every
  pin that becomes relevant, deduplicated by link (which is also what terminates the walk).
- "Ramp production up to the summed demand" needs no special rule: it falls out of the existing
  free-variable rule ("keep the current value") applied to the untouched consumer.

### Task 2b — proportional re-split when the shared pin's total changes — DONE
- Driving the shared pin (60 -> 90 on a 40/20 fan-out) re-splits in the prior ratio (60/30).
- **Bug found while implementing:** a first cut broke the "wire up a new consumer" case. When the
  solver happens to pick a link variable as the free one during that solve, the ratio rule would
  drag both branches back to their old ratio and fight the new consumer's constraint. Fix: a
  branch whose far pin IS the solve's constraint pin is excluded from the re-split and folded into
  the equation - the same `already_constrained` concept CustomSplitter uses, which I had wrongly
  dismissed as unnecessary during design.

### Task 3 — the lock no longer crosses a fan-out — DONE
- `Pin::SetLocked` only propagates across an edge that is the sole link on *both* sides. A lock
  fixes a pin's total, and under fan-out says nothing about how that total divides.
- Same guard on `CreateLink`'s lock sync.

### Task 4 — `CreateLink` chooses the constraint pin — DONE
- When one side already had links, the *newly wired* end drives the solve (pull), so the fanning
  pin absorbs the sum. Without this, `CreateLink` pushes from the source and squeezes the new
  consumer to the rate the source already carried.
- Verified the test discriminates: disabling the rule makes it fail.

### Task 5 — persistence — DONE
- Each link serializes its own `rate` (`{num, den}`, same shape nodes already use), in both
  `SessionSerializer` and `GroupNode`.
- Old saves have no `rate`: there every pin held at most one link, so the edge's rate is its end
  pin's rate, and reconstructing it that way reproduces the original graph. Tested against a real
  serialized session with the field stripped, not a mock.

### Task 6 — UI — DONE
- `DragLink` no longer rejects an occupied pin. New rejection added: the same pin pair twice
  (a duplicate edge would stack invisibly and double the flow).
- `RenderLinks` red-check changed from "the two ends differ" (which a fan-out legitimately does)
  to "a pin's rate does not equal the sum of its links".
- Hovering a belt shows the rate that edge carries - the only place the number exists when a
  fan-out feeds a fan-in.

### Task 7 — GroupNode with an internal fan-out — DONE (partially testable)
- **Plan premise was wrong:** `CreateInsOuts` never used "pin has no link" as its criterion; it
  balances by item (produced vs consumed inside the group). So a fan-out nets out exactly like a
  single belt carrying the same total, and no change was needed there.
- The real fix was already made in Task 1: `GroupSelectedNodes` now iterates *all* of a pin's
  links (over a copy, since the callee mutates them). Before, a pin with fan-out would have had
  one link processed and the other left dangling at a node that had left the graph.
- **Limit:** `GroupSelectedNodes` calls `ax::NodeEditor::IsNodeSelected`, which needs a live
  editor context, so the group-boundary cut is not unit-testable headlessly. Covered by a test on
  GroupNode wrapping an internal fan-out; the boundary cut needs manual verification.

## Status: all tasks implemented. Suite 242/242 green (232 baseline + 10 new).
## Remaining: manual GUI verification (below).

---

# Progress: Snapshot Real-Efficiency Rates & Belt-Throughput Overlay

Spec: `docs/superpowers/specs/2026-06-30-snapshot-efficiency-and-throughput-design.md`

Two linked Factory Snapshot features:
- **A — Apply real machine efficiency** (toggle, default ON): fold each machine's
  save-measured productivity into its rates, then propagate.
- **B — Belt-throughput overlay** (toggle, default OFF): draw items/min on links.

TDD throughout. No git ops (per user preference).

## Status: ALL COMPONENTS IMPLEMENTED & TESTED (GUI toggle + overlay = manual verify)
Full suite **232/232** C++ + **23/23** JS green. App builds. Verified on the
user's save `teste-1.2-beta_autosave_2.sav` (161/280 producers below 100%).

## Component status
- [x] **C3a — Extract `PropagateRates` (pure domain fn)**  ← DONE
  - New `domain/snapshot/rate_propagation.{hpp,cpp}`; loop moved verbatim from
    `sav_import.cpp` (former lines ~1705–1822). Signature:
    `PropagateRates(nodes, links, mixed_item_nodes = {})`.
  - `IsStationFuelPin` copied as a file-local helper in the new .cpp.
  - Importer's `mixed_item_nodes` changed to `unordered_set<const Node*>`; inline
    loop replaced by one `PropagateRates(...)` call. The separate downstream
    "connection consistency / demand-pullback" pass stays in `sav_import.cpp`
    (got its own local `kMaxRatePropagationIters`).
  - Tests: `tests/test_rate_propagation.cpp` (5 cases) — merger sum, splitter
    divide among connected outs, link copy, multi-hop convergence, craft input
    pin NOT overwritten. Wired into CMake (fc-core + fc-tests).
  - RED verified (4/5 fail on stub) → GREEN → REFACTOR. **Full suite 229/229.**

- [x] **C1 — `readProductivity` in JS wrapper** — DONE. `wrapper_core.js` +
      exported; `wrapper.js` emits `efficiency` on every building entry. 5 new
      `wrapper_core.test.js` cases (last/current window, clamp, fallback, shapes).
- [x] **C2 — Importer plumbing** — DONE. `SavImport::Building.efficiency` parsed
      from JSON; `BuildOptions::apply_efficiency` (default false). Producer seed
      uses `SeedRate(clock, efficiency, apply)` = `ClockToFraction(clock) *
      EfficiencyToFraction(efficiency)` (0→0, not clamped to 1 like clock).
      Test: rates scale only when apply on; downstream (merger) also scales.
- [x] **C3b — App rate pipeline** — DONE, but via **re-import** not `ApplyRates`
      (see decision below). `FactorySnapshotApp::RebuildSnapshot()` re-runs
      `BuildSnapshotFromJson` from retained wrapper JSON with the session's
      apply_efficiency, preserving the camera (`needs_position_apply`).
- [x] **C5 — Persistence & UI** — DONE. `apply_efficiency` (default ON) /
      `show_throughput` (default OFF) in `FactorySnapshotSession` (additive keys,
      round-trip + default tests). Options-tab "Rates" section: two checkboxes +
      tooltips; efficiency toggle → `SaveSession()` + `RebuildSnapshot()`;
      reset-to-defaults restores both.
- [x] **C4 — Throughput overlay** — DONE (code); manual visual verify pending.
      `RenderThroughputOverlay()` draws each visible edge's source-pin
      `current_rate` + "/min" on a pill at the pin-center midpoint (screen
      space), clipped to the canvas. `pin_centers` captured during the node pass
      via `RecordPinCenter` (works at all zooms incl. collapsed 1px markers).

## Notes / decisions
- **Save productivity field (verified on real 1.2 save):** there is NO direct
  `mCurrentProductivity`. Productivity = `mLastProductivityMeasurementProduceDuration
  / mLastProductivityMeasurementDuration` (last completed window; present on
  ~all machines), current-window pair as fallback, else 1.0. See
  [[snapshot-save-productivity-field]].
- **Deviation from spec (approved by engineering judgment):** the spec's
  `ApplyRates()` (re-seed + forward `PropagateRates` only) would DROP the
  importer's second "demand-pullback" consistency pass (sav_import.cpp ~1732+,
  entangled with vehicle-route balancing), changing the default view for
  everyone and making Feature B less accurate. Instead the efficiency toggle
  **re-imports** from the retained wrapper JSON, reusing the ENTIRE importer
  pipeline (forward prop + pullback + vehicle + warnings + flow report) so both
  modes are internally consistent. `PropagateRates` extraction (C3a) is still
  used by the importer.
- `Pin::item` is `const Item*` (pointer non-const) → item pass-through assignment
  is legal; topology never mutated.

## Manual verification still to do (GUI, per spec)
1. Import the save in the Factory Snapshot tool.
2. Options tab → "Apply real machine efficiency": toggling OFF shows nominal
   rates; ON shows reduced rates on the 161 sub-100% machines + shifted Resources
   totals. Camera stays put across the toggle.
3. "Show belt throughput": items/min pills appear on belts at all zooms and
   match the producing machine's (efficiency-adjusted) output.

---

## (Previous, completed) hide-logistics-by-item feature — DONE
Extend per-item production filter so hiding an item also hides splitters/mergers/
stations/storages carrying only that item (reroute/bypass). See
`NodeLogisticsHiddenByItems` + `NodeProductionHidden` in node_display.{hpp,cpp}.
