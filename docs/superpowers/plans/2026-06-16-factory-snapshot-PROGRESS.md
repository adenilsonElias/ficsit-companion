# Factory Snapshot — Execution Progress

Plan: `docs/superpowers/plans/2026-06-16-factory-snapshot.md`
Execution mode: subagent-driven (same session). **No git commits** (per user git-preference — progress tracked here, not via commits). Branch: `develop`.

## Status legend
⬜ not started · 🔄 in progress · ✅ done (impl + spec review + quality review)

| # | Task | Status | Notes |
|---|------|--------|-------|
| 1 | SaveSource struct + JSON round-trip (+test) | ✅ | 3/3 tests pass. Note: 5 sav fields overlap VehicleMapSession — intentional for migration step 1; per-tool watch UI keeps its own copy. |
| 2 | FactorySnapshotModel data holder | ✅ | builds clean; added link.hpp for complete-type deletion. |
| 3 | FactorySnapshotBuilder (ParseResult/JSON) +tests incl. 120/min regression | ✅ | 3/3 pass; added err.clear() on success (quality fix). |
| 4 | Builder warnings + no-GraphModel-mutation tests | ✅ | 5/5 pass; GraphModel needs IEditorBackend& + uses .nodes member (not GetNodes()). |
| 5 | FactorySnapshotSession round-trip (+test) | ✅ | 3/3 pass; mirrors VehicleMapSession idiom. |
| 6 | BaseApp::LoadFromWrapperJson hook | ✅ | done together (6-8); app links clean. |
| 7 | VehicleMapApp consumes shared JSON | ✅ | LoadSavFile split; emscripten guard stays only in LoadSavFile. |
| 8 | ProductionApp consumes shared JSON | ✅ | delegates to existing ImportSavFromJson (additive). |
| 9 | FactorySnapshotApp (list-first read-only UI) | ✅ | links clean; read-only verified. Deferred minors: per-keystroke save, topology loop-bound NOTE, error color. (const-row suggestion rejected: GetStringFraction is non-const.) |
| 10 | Shared load bar + 3rd tab + distribute in AppHost | ✅ | one Load feeds all 3 tools; host watcher OPT-IN (sav_watch_enabled, default off) to avoid competing with per-tool watchers. |
| 11 | Whole-suite green + final build + spec status update | ✅ | 128/128 tests pass; ficsit-companion.exe links; spec marked Implemented (Phase 1). |

## Final status — ALL TASKS COMPLETE ✅
- 128/128 tests pass (incl. 11 new: 3 save_source + 5 snapshot_builder + 3 snapshot_session).
- `ficsit-companion.exe` + `fc-tests.exe` link clean.
- Final whole-feature review found one **[Critical]** runtime crash (shared Load called
  `ProductionApp::ImportSavFromJson` → `ax::NodeEditor::SetNodePosition` outside the editor
  context → null deref). **Fixed**: `ProductionApp::LoadFromWrapperJson` now queues the JSON;
  `DrainPendingImports` (runs inside the editor context) applies it. Re-verified: green.
- ⚠️ **Not yet runtime-verified in the GUI** (no display in this session): clicking the shared
  "Load" and seeing all three tabs populate. Recommend a manual smoke test.
- No git commits made (per git-preference). All work is in the working tree.

## Deviations from plan
- Plan steps say "Commit" / use BASE_SHA..HEAD_SHA; per git-preference we do NOT commit.
  Reviewers inspect the working-tree changes for each task instead.
- Task 10: added SaveSource persistence (saved/save_source.json) + a "World" input and "Auto-watch" checkbox to the shared bar (beyond the plan's bar) so the shared watch config is reachable/persistent.

## Migration step 2 — single global load path (DONE 2026-06-17)
User report: the shared/global `.sav` load did not populate Vehicle Map / Factory
Snapshot, while the modeler's own import worked. Root causes: (a) the global bar did
not carry the modeler's import options (layout / spacing / connect-routes) nor a node
path, so tools imported with mismatched defaults; (b) Vehicle Map had a render-time
reload (`initial_load_done` → re-import `last_sav_path`) + its own SaveWatcher that
competed with / overwrote the global push.

Fix — consolidated to ONE load path (the global AppHost bar):
- `SaveSource` gained the shared import options (`layout_mode`, `world_spacing_scale`,
  `connect_vehicle_routes`) + `ToBuildOptions()`; round-trip + BuildOptions tests added.
- `BaseApp::LoadFromWrapperJson(json, options)` now threads the shared options to every
  tool; `AppHost::DistributeJson` derives them from `SaveSource` once per load.
- Global bar exposes layout / world-spacing / connect-routes + a desktop node-path field,
  and on web a single "Import .sav..." button (the `waitForSavFileInput` JS moved into
  main.cpp). All three tools build identically from one parse.
- Load UX (desktop, restored to the modeler's feel per user feedback): a **Save folder**
  input + a **dropdown of the .sav files** in that folder (newest first; `AppHost::
  RefreshDiscoveredSaves`) — picking one loads it. Plus a **Reload** button and **Auto-watch**.
  The raw single-path field / Load+Latest buttons were removed (confusing). `LoadSav` also
  resolves a directory argument to the newest .sav (guards the wrapper's `EISDIR` error).
- **Removed the modeler's save load:** ProductionApp's "Save Import" controls + web
  "Import .sav" button + its own SaveWatcher / `ImportSavFile` / `RunSavParserDesktop` /
  `RefreshDiscoveredWorlds`. `RenderSavImportSection` is now a read-only last-import
  status (error + warnings popup) only.
- **Removed Vehicle Map's per-tool load:** its Load section controls, SaveWatcher,
  `LoadSavFile` / `DrainPendingImports` / `RefreshDiscoveredWorlds`, and the render-time
  reload. `RenderLoadSection` is now a read-only status.
- Verified: 130/130 tests pass; `ficsit-companion.exe` + `fc-tests.exe` link clean.
- Behavior change: tools no longer auto-restore the imported save on startup; the user
  clicks the global **Load** (or enables global **Auto-watch**). Still needs a GUI smoke test.

## Factory Snapshot read-only node graph (DONE 2026-06-17)
Spec: `docs/superpowers/specs/2026-06-17-factory-snapshot-graph-view-design.md`
Plan: `docs/superpowers/plans/2026-06-17-factory-snapshot-graph-view.md`
User report: the Snapshot showed only the resource-flow list; wanted a modeler-style node
graph, read-only, no balancer. Executed subagent-driven (3 tasks, per-task review + final
opus whole-feature review). No git commits (git-preference).
- New pure domain helper `NodeDisplayName(const Node&)` (+ `test_node_display`) for node titles.
- `FactorySnapshotApp` hosts its own `imgui-node-editor` context (SettingsFile=nullptr);
  `RenderGraphCanvas`/`RenderSnapshotNode`/`RenderSnapshotLinks` draw nodes (title + per-pin
  item & imported `current_rate`) and links read-only. No edit/create/delete handlers; balancer
  never called. `needs_layout_apply` one-shot applies imported positions + `NavigateToContent`
  on load (verified correct after `End()` for vendored node-editor 0.9.4).
- Layout: status line + 340px left panel (topology, warnings, collapsible resource flow w/ ScrollX)
  + graph main / empty-state. Pan/zoom/drag allowed, never persisted.
- Final-review fix: vehicle-station `plug` pins are now submitted so plug↔plug route links render.
- Verified: 131/131 tests pass; `ficsit-companion.exe` links clean. Still needs a GUI smoke test
  (load a save, open Factory Snapshot tab; confirm nodes/links/rates draw and fit-on-load).

### Item icons on nodes (DONE 2026-06-17)
Spec: `docs/superpowers/specs/2026-06-17-snapshot-node-icons-design.md`. For zoom-out
recognition of productions.
- New pure helper `NodePrimaryItem(const Node&)` in `domain/node_display.*` (+ unit test):
  first non-null output pin item, else first input pin item, else null.
- `RenderSnapshotNode`: header draws the product icon (`Item::icon_gl_index` via ImGui::Image),
  sized `lineHeight * clamp(1/GetCurrentZoom(), 1, 3)` so it grows when zoomed out; each pin now
  draws its item icon (helper `DrawPinIcon`, falls back to the circle marker when no item/texture).
- Verified: 132/132 tests pass; `ficsit-companion.exe` links clean. GUI smoke test still pending.

### Fix: splitter "strange values" were exact fractions (2026-06-18)
User report: splitters show strange values from division. Root cause (display, not math):
the snapshot rendered rates with `FractionalNumber::GetStringFraction()` (exact `num/den`),
so a splitter dividing by N showed e.g. `100/3` → `"100/3/min"`. The division is correct;
the fraction display looked broken, and splitters are where those fractions originate.
Fix: render `GetStringFloat()` (decimals, matching the modeler) in `PinLabel` and the snapshot
resource-flow table. `ficsit-companion.exe` links clean.

### Miner purity in node title (2026-06-18)
Extractor nodes now show purity in the title parenthetical, e.g. `Miner Mk.2 (Iron Ore, Pure)`.
Added `bool ExtractorNode::SupportsPurity()` (wraps the spec flag; Water Extractor = false);
`NodeDisplayName` Extractor case appends Impure/Normal/Pure when supported. TDD test added
(asserts the `(Iron Ore, Pure)` parenthetical; water has no purity word). 133/133 tests pass.

### Left-panel tabs + click-to-producer (2026-06-18)
Spec: `docs/superpowers/specs/2026-06-18-snapshot-left-panel-and-jump-design.md`.
- Left panel is now a TabBar: **Resources** (flow table, fills the panel) + **Info** (topology +
  warnings). The flow table sizes to the remaining region instead of a fixed 12-row box.
- Click a resource row → jump the graph to a producer. Pure helper `NodesProducingItem(nodes,
  item_name)` (Craft/Extractor with the item on an output; excludes pass-through) added to
  `domain/node_display.*` (+ unit test). Clicking sets `nav_item`; `RenderGraphCanvas` consumes it
  (SelectNode + NavigateToSelection) inside the editor context, cycling through producers on
  repeated clicks (resets when a different item is clicked).
- Verified: 134/134 tests pass; `ficsit-companion.exe` links clean. GUI smoke test pending.

## Deferred follow-ups (out of this plan's scope)
- **(done — see Migration step 2 above)** Consolidate save watchers / per-tool load.
- Snapshot editor accumulates dead nodes across many re-imports in one session (harmless;
  excluded from draw + fit). Optional: recreate the editor context on import if it ever matters.
- FactorySnapshotApp minors: per-keystroke SaveSession; topology loop-bound NOTE comment; error color 0.5→0.4 green channel.
- VehicleMapSession's 5 sav fields overlap SaveSource (migration step 2 can retire the duplication).
