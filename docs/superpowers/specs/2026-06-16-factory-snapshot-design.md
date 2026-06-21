# Design: Read-Only Factory Snapshot vs Editable Modeler

**Date:** 2026-06-16
**Status:** Implemented (Phase 1 — shared load + list-first snapshot). See
`docs/superpowers/plans/2026-06-16-factory-snapshot.md` and its PROGRESS file.
**Scope:** Split `.sav` loading from production modeling so imported save-game
state is read-only and the modeler remains a clean editable planning workspace.

> **User decision (supersedes §4):** Rather than keeping the Modeler isolated
> from `.sav` watch/import, the user asked for a single shared load that feeds
> *all three* tabs the same save. The Modeler therefore also receives the shared
> load — but additively (`ImportSavFromJson` appends a GroupNode; it does not
> overwrite the current plan), so the planning workspace is not destroyed.
> The read-only Factory Snapshot ships as a **list-first** view (topology counts
> + resource-flow table + warnings); the read-only node-editor canvas of §12 and
> migration steps 2–3 (consolidating/removing per-tool watchers) are deferred.

---

## 1. Motivation

The current `.sav` import path builds an editable production graph inside
`ProductionApp`. That mixes two different jobs:

- **Factory snapshot:** show what exists in the current save-game base.
- **Modeler:** let the user design and balance planned production.

Those jobs have different correctness rules. A save-game base can contain
partial manifolds, buffers, transient vehicle flow, disconnected machines,
over-production, under-production, and import uncertainty. A modeler graph
should be intentional, editable, and driven by exact user-selected rates.

The split avoids importer balancing logic rewriting imported reality, and avoids
modeling assumptions being confused with the actual factory state.

---

## 2. Product Shape

Add a new top-level read-only tool named **Factory Snapshot**.

The app has three distinct surfaces:

1. **Modeler** (`ProductionApp`): editable production planning graph.
2. **Vehicle Map** (`VehicleMapApp`): read-only vehicle/station map from `.sav`.
3. **Factory Snapshot** (new): read-only machine/logistics graph and resource
   status from `.sav`.

Factory Snapshot is the default destination for `.sav` import. Modeler no longer
automatically receives imported save-game nodes as its working graph.

---

## 3. Read-Only Factory Snapshot

Factory Snapshot loads a `.sav` through the existing Node wrapper and displays
the imported factory as observed state.

It should show:

- Imported machines, generators, extractors, storage, stations, buffers.
- Belt and pipe connections, including synthetic pipe manifolds when needed.
- Resource flow totals: produced, consumed, net, surplus/deficit.
- Import warnings and skipped/uncertain topology.
- Optional world-layout positioning for inspecting the base.

It should not allow:

- Creating/deleting nodes or links.
- Editing machine rates, recipes, pins, or locks.
- Saving the snapshot as the modeler session.
- Running interactive modeler solves that rewrite imported rates.

Snapshot rates are descriptive. They can be calculated from machine recipes,
extractor output, and imported topology, but they should not imply that the save
is balanced.

---

## 4. Editable Modeler

Modeler keeps the current production-planning behavior:

- User-created nodes and links.
- Manual recipe/rate edits.
- Rate solver behavior on interactive connections.
- `.fcs` session save/load for plans.
- Grouping, duplication, and manual layout.

The modeler is not automatically overwritten by `.sav` watch/import. A user can
keep a plan open while the Factory Snapshot reloads from the latest save.

---

## 5. Bridge Between Snapshot and Modeler

The first version should keep the bridge minimal:

- A clear visual/action boundary: Snapshot is read-only, Modeler is editable.
- No automatic synchronization between the two.

Later bridge features can be added deliberately:

- **Copy selected area to Modeler:** creates editable modeler nodes from a
  selected snapshot subgraph.
- **Create plan from recipe chain:** starts a clean modeler plan from selected
  outputs or deficits.
- **Compare plan vs snapshot:** shows whether the modeler plan covers factory
  deficits or expands current production.

These are out of scope for the first split.

---

## 6. Architecture

### Current State

`ProductionApp::ImportSavFromJson` currently:

1. Parses wrapper JSON with `SavImport::ParseWrapperJson`.
2. Builds editable `Node`/`Link` objects with `SavImport::BuildGraph`.
3. Wraps/imports them into the modeler canvas.

### Target State

Introduce a Factory Snapshot app that owns its own imported state.

Suggested new modules:

- `FactorySnapshotApp` in `app/`: UI and top-level load/watch flow.
- `FactorySnapshotSession` in `infra/`: persisted watch path, selected world,
  filters, layout/camera state.
- `FactorySnapshotModel` in `domain/`: read-only imported graph model, warnings,
  resource-flow report input.

Reuse existing modules where practical:

- `SaveWatcher`
- `DiscoverWorldNames`
- `SavRunner` / wrapper execution
- `SavImport::ParseWrapperJson`
- Most of `SavImport::BuildGraph`, if adjusted to support read-only snapshot
  output cleanly
- `BuildResourceFlowReport`

Factory Snapshot may initially reuse `Node`/`Link` structures for rendering, but
the app must not expose graph mutation operations. If reuse keeps causing
planner assumptions to leak into snapshot behavior, introduce explicit snapshot
DTOs instead.

---

## 7. Data Flow

Factory Snapshot load flow:

1. User chooses a `.sav` or enables watch mode.
2. Wrapper parses `.sav` and emits JSON.
3. C++ parses JSON into `SavImport::ParseResult`.
4. Snapshot builder creates read-only imported state.
5. UI displays topology, warnings, and resource-flow report.

Modeler flow remains separate:

1. User opens or creates a `.fcs` plan.
2. User edits nodes/links.
3. Interactive solver updates planning rates.
4. Modeler session saves independently.

There is no implicit data flow from Factory Snapshot to Modeler.

---

## 8. UI Behavior

The app shell should expose Factory Snapshot as a separate tool next to Modeler
and Vehicle Map. Exact navigation can follow the existing top-level app pattern
in `main.cpp`.

Factory Snapshot UI should include:

- Load/watch controls similar to Vehicle Map.
- A status line with last loaded save and warning count.
- A read-only graph/map canvas or list-first view.
- Resource flow table with filters for all/deficit/surplus.
- Warnings panel with copyable diagnostics.

Read-only affordances matter: the UI should not look like an editable planning
canvas. Avoid showing add-node menus, delete controls, lock toggles, or editable
rate controls in Snapshot.

---

## 9. Migration Path

Step 1: Add Factory Snapshot without removing current modeler import.

- Keep current Modeler `.sav` import behind existing controls while Snapshot is
  introduced.
- Add clear labels if both remain temporarily.

Step 2: Move `.sav` watch/import defaults to Factory Snapshot.

- Modeler keeps `.fcs` plan load/save.
- If modeler import remains, label it as an explicit conversion/import action,
  not as "load save".

Step 3: Remove or hide automatic `.sav` import into Modeler once Snapshot covers
the base-inspection workflow.

---

## 10. Testing

Domain/infra tests:

- Snapshot session round-trip.
- `.sav` wrapper JSON parses into snapshot state.
- Snapshot build preserves producer-side rates on pipe junction inputs.
- Resource-flow report can be built from snapshot state.
- Snapshot load does not mutate a modeler `GraphModel`.

App-level smoke tests:

- Factory Snapshot loads example wrapper JSON.
- Warnings are retained and displayed.
- Modeler session remains unchanged after Snapshot reload.

Regression target:

- A Water Extractor producing 120/min into a Pipe Junction should display 120 on
  the producer-side junction input in Snapshot.

---

## 11. Non-Goals

- No pressure/head simulation for pipes.
- No full synchronization between snapshot and modeler.
- No automatic generation of an optimized production plan from a save.
- No editing imported save-game entities in the Snapshot screen.
- No changes to the Satisfactory save file itself.

---

## 12. Initial Implementation Decisions

- Factory Snapshot should initially reuse ImGui Node Editor for a read-only
  topology canvas because the imported factory graph is already represented as
  nodes and links. The UI must disable edit interactions.
- `SavImport::BuildGraph` should not be overloaded with more UI policy. Add a
  small snapshot-builder layer that can reuse its parsing and graph-building
  pieces while keeping read-only snapshot ownership separate from `GraphModel`.
- Modeler may keep a manual "Import from `.sav` as editable graph" action during
  migration, but automatic save loading/watch belongs to Factory Snapshot.
