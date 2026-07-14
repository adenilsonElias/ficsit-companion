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
