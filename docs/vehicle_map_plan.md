# Plan: Vehicle & Station Logistics Map (new decoupled tool)

## Context

Satisfactory 1.2 added recordable vehicle paths. In a large save these become hard
to track: which trucks/trains exist, what route each runs, which stations a route
visits, and which vehicles serve a given station. The user wants a **second tool**
inside ficsit-companion that answers those questions on a **2D world map**, while
staying **decoupled from the existing production planner ("modeler")**.

This is feasible because the repo already has an in-progress `.sav` import pipeline:
`tools/sav_import/wrapper.js` (uses `@etothepii/satisfactory-file-parser`) emits JSON
that C++ parses (`sav_import.cpp`), and `SaveWatcher` auto-detects autosaves.
`docs/save_game.md` confirms the relevant actors exist in the sample save:
`Build_TruckStation_C`, train/railroad stations, `VehicleSpecialProperties` (19
vehicles), `Build_VehiclePath_Universal` (627), `Build_VehiclePathNode_Default` (489).

**Note on SC-InteractiveMap:** it is a web (JS/Leaflet) app and cannot be embedded in
this C++/ImGui desktop app. We reuse only (a) the same parser package we already use,
(b) its world→pixel coordinate constants, and (c) a static world-map background image.
The map itself is rendered with ImGui's `ImDrawList` (custom canvas).

### Confirmed decisions
- **Background:** static world-map PNG behind the canvas, aligned to world bounds.
- **Data refresh:** reuse `SaveWatcher` for auto-refresh on autosave + a manual "Load .sav" button.
- **Scope:** road vehicles (Truck/Tractor/Explorer/Cyber Wagon) **and** trains (train
  stations + trains + rail networks). v1 is a **read-only viewer** (no editing of
  paths/stations) — matches the "I want to *see*…" framing.
- **v1 interaction must include** an *item-centric query* ("show everything that moves
  Iron Ore") and *fly-to-selection* (double-click centres + zooms the camera). These
  two are what make the tool answer questions rather than just draw dots — see the
  **Usability** section below.

### Open questions still to discuss (parked — user ran out of time)
- Remaining questions about the plan the user wanted to raise before implementation.
- Whether to source the world-map background image now or start on a blank grid and add it later.
- Confirm desktop-first; defer web (Emscripten) parity.

---

## Approach

### 1. Tool switching (decoupling) — touch `main.cpp` only
Add a thin top tab/segmented switcher in the shared `"Ficsit Companion"` window in
`main.cpp` (`Render`, around lines 63–76). Instantiate **both** apps and render the
active one:

```cpp
ProductionApp production_app;
VehicleMapApp  vehicle_app;
BaseApp* apps[] = { &production_app, &vehicle_app };
// small tab bar picks active index; then: apps[active]->Render();
```

`VehicleMapApp` is a new `BaseApp` subclass (implements `RenderImpl()` + `SaveSession()`).
`ProductionApp::RenderImpl()` is **left untouched** — no coupling. Same switcher works on
the Emscripten path (the existing `WindowApp{window, app}` becomes the active pointer).

### 2. Discovery step FIRST (de-risk the unknown)
`save_game.md` documents production topology but **not** vehicle→path→station linkage.
Before coding the importer, write a throwaway Node dump script (under `tools/sav_import/`)
that runs the existing parser on `example_save.sav` and prints the raw shape of:
`Build_TruckStation_C`, train/railroad station classes, vehicle actors
(`VehicleSpecialProperties`), `Build_VehiclePath_Universal`,
`Build_VehiclePathNode_Default`, and railway track/station actors. Goal: nail down
- station: display name, world translation, item(s) loaded/unloaded;
- vehicle: type, current translation, reference to its target/path list;
- path: ordered waypoint nodes (the polyline) and how a path's target points map to stations;
- trains: how train stations chain along rail track actors.

Record findings as a new section in `docs/save_game.md` (vehicle/path topology).

### 3. Data extraction — extend `wrapper.js` additively
Add an **additive** top-level `logistics` block to `wrapper.js` output (single parse
pass, low risk — existing `buildings`/`belts` consumers ignore it; the new tool reads
only `logistics`):

```jsonc
"logistics": {
  "stations": [ { "id", "kind": "truck"|"train", "name", "pos": [x,y,z],
                  "load_items": [..], "unload_items": [..] } ],
  "vehicles": [ { "id", "type": "truck"|"tractor"|"explorer"|"cyberwagon"|"train",
                  "name", "pos": [x,y,z], "path_id" } ],
  "paths":    [ { "id", "kind": "road"|"rail",
                  "waypoints": [[x,y],..], "station_ids": [..] } ]
}
```

Helper functions mirror the existing `classifyBuilding`/`followBeltChain` style.
*(Alternative considered: a separate `vehicle_export.js`. Rejected for v1 — it would
re-parse the 2.5 MB save a second time per autosave. Additive output keeps one parse.)*

### 4. New C++ module: `vehicle_map.hpp` / `vehicle_map.cpp`
Independent of `node.hpp`/`sav_import.hpp`. Mirrors the `SavImport::ParseWrapperJson`
pattern but produces a flat logistics model:

```cpp
namespace VehicleMap {
  struct Station { std::string id, name; int kind; ImVec2 pos;
                   std::vector<const Item*> load, unload;
                   std::vector<std::string> vehicle_ids; };
  struct Vehicle { std::string id, name; int type; ImVec2 pos; std::string path_id; };
  struct Path    { std::string id; int kind; std::vector<ImVec2> waypoints;
                   std::vector<std::string> station_ids; };
  struct Model   { std::vector<Station> stations; std::vector<Vehicle> vehicles;
                   std::vector<Path> paths; std::string error; bool ok; };
  Model ParseLogisticsJson(const std::string& wrapper_json);
}
```

Cross-links (station↔vehicle↔path) are resolved here from the JSON ids. Item display
names resolve via the global `Data::Items()` (read-only shared singleton).

### 5. Shared subprocess runner (light refactor)
The "run node on wrapper.js, capture stdout JSON" logic currently lives as a private
`ProductionApp` method (`production_app.cpp:4789–4888`, `_popen`/`popen`,
`QuoteArgForShell`, stderr tempfile). Extract it into a small free function so both
tools share it without coupling:

```cpp
// new sav_runner.hpp/cpp (or append to utils)
std::string RunSavWrapper(const std::string& sav_path,
                          const std::string& node_exe, std::string& err);
```
`ProductionApp` calls the extracted function (behavior unchanged); `VehicleMapApp` calls
it too. Keep the Emscripten `web_loader.js` path as-is for web.

### 6. `VehicleMapApp` — UI & rendering
`RenderImpl()` layout: left panel (load/watch controls + station/vehicle list) + main
**map canvas** (`ImGui::BeginChild` + `ImGui::GetWindowDrawList()`), no NodeEditor.

- **World→screen transform:** `screen = (world * zoom) + pan`. Mouse-wheel zoom about
  cursor; drag to pan. Calibrate world bounds from SC-InteractiveMap's known constants
  against the background image.
- **Background:** `LoadTextureFromFile("icons/world_map.png")` drawn via
  `draw_list->AddImage(...)` mapped to world bounds. (Ship the PNG under `assets/` or
  `icons/`; if absent, `LoadTextureFromFile` already falls back to a placeholder, so a
  blank-grid fallback is automatic.)
- **Paths:** `AddPolyline(waypoints)` — road vs rail styled differently.
- **Stations:** square marker + optional building icon; label on hover.
- **Vehicles:** triangle/icon marker at current pos.
- **Interaction (the core asks):** click a station → highlight its `vehicle_ids` and the
  paths through it, and list connected vehicles in a side panel. Click a vehicle →
  highlight its `path_id` polyline and the stations it serves. Hover → tooltip with name
  and items. (The full interaction/usability model — search, filters, item-centric query,
  fly-to, layers, de-overlap — is specified in the **Usability** section below.)
- **Refresh:** `SaveWatcher` (reused, own instance) auto-reparses on autosave; manual
  "Load .sav…" button. `SaveSession()` persists last save path / watch dir / camera /
  selection / active filters.

### 7. Build wiring
Add `include/vehicle_map.hpp`, `src/vehicle_map.cpp`, `include/vehicle_map_app.hpp`,
`src/vehicle_map_app.cpp`, and `sav_runner.*` to `ficsit-companion/CMakeLists.txt`
(alongside the already-wired `sav_import.*` / `save_watcher.*`). Ship the world-map PNG
and include it in the post-build copy / Emscripten preload like other `icons/`.

---

## Usability (UI/UX) — implementation plan

This is the interaction layer for `VehicleMapApp`. It is **additive to the data model in
task 4** — most features are derived/index state computed once per load, plus per-frame
view logic. Nothing here changes `wrapper.js` or the C++ parse model beyond a few extra
helper indices.

### View state (held on `VehicleMapApp`, persisted in `SaveSession()`)
```cpp
struct ViewState {
  ImVec2 pan;  float zoom = 1.0f;          // camera
  std::string  sel_station, sel_vehicle;   // current selection (one of)
  std::string  sel_item;                   // active item-centric query (empty = off)
  std::string  search;                     // free-text filter
  uint32_t     type_mask = ~0u;            // vehicle-type filter bitset
  struct Layers { bool roads=1, rails=1, vehicles=1, stations=1, labels=1; } layers;
  bool color_by_item = false;              // legend mode: type (default) vs item
};
```
Add a `flyto_target` (optional ImVec2 + target zoom) + a short eased-tween timer so
fly-to animates rather than snapping.

### Derived indices (built once in `VehicleMap::Model` after parse, task 4)
- `std::unordered_map<std::string, size_t>` id→index for stations / vehicles / paths
  (O(1) cross-link resolution; replaces linear scans).
- `item → {station_ids, vehicle_ids, path_ids}` multimap — powers the **item-centric
  query** and the item filter dropdown. Built by walking each station's load/unload and
  fanning out through its vehicles and paths.
- `orphans`: precomputed list of stations with empty `vehicle_ids` and vehicles with
  empty/unresolved `path_id` (the **broken-route detector**).

### Feature-by-feature

**1. Search + filter bar** (top of left panel)
- `ImGui::InputTextWithHint` for name search; `ImGui::Combo`/checkbox row for vehicle-type
  mask; an item `ImGui::Combo` populated from the item index (with an "All" entry).
- A single `IsVisible(entity)` predicate combines search ∧ type_mask ∧ item filter; the
  list and the canvas both query it so they never diverge.

**2. Item-centric query (v1 headline feature)**
- Selecting an item in the combo, or clicking an item chip in a station tooltip, sets
  `sel_item`. Render pass then: full-opacity for entities in the item index for that item,
  dimmed (low alpha) for the rest; draw connecting paths emphasised. A clearly-labelled
  "Clear" chip resets it. Reuses the same highlight machinery as selection.

**3. Bidirectional list↔map sync + fly-to (v1)**
- Hover a list row → set a transient `hover_id` → marker drawn highlighted; hover a marker
  → same row scrolls into view (`ImGui::SetScrollHereY` when the list is rebuilt).
- Single click = select; double click (row or marker) = select **and** set `flyto_target`
  to the entity world pos at a comfortable zoom. `Esc` clears selection; clicking empty
  canvas clears selection.

**4. Orphan / broken-route detection**
- A collapsible "Issues" group in the left panel listing orphan stations / pathless
  vehicles from the precomputed `orphans`. Each entry is click-to-fly-to. A small badge on
  the affected marker (e.g. ring/`!`) so it's visible on the map too.

**5. Layer toggles + legend**
- Checkbox row bound to `ViewState::Layers`; each draw pass guards on its flag.
- Legend panel: swatches for road vs rail, each vehicle type, and station; in
  `color_by_item` mode the legend lists the items currently in view with their colours
  (stable colour from a hash of the item id).

**6. Marker de-overlap**
- Before drawing, bucket markers by rounded screen position; for buckets with >1 marker,
  fan them out on a small circle (deterministic angle by id) so a parked vehicle no longer
  hides its station. Purely a render-time offset; hit-testing uses the offset position.

**7. Camera niceties**
- "Fit all" button: compute world AABB over visible entities → set pan/zoom to frame it.
  "Reset view" returns to default. Live world-coordinate readout (`world = (mouse-pan)/zoom`)
  in a corner overlay.
- Zoom-dependent detail: skip `labels` below a zoom threshold; decimate path polylines
  (draw every Nth waypoint, or a screen-space tolerance) when zoomed out to keep the
  627-path / 489-node case smooth. Cull entities whose screen pos is outside the canvas
  rect before drawing.

**8. Rich tooltips / detail panel**
- Hover tooltip: station → name, items in/out, serving-vehicle count; vehicle → name, type,
  ordered stop list, path length, fuel if the actor exposes it.
- On selection, a detail sub-panel (below the list) shows the same info expanded, with the
  serving vehicles / served stations as click-to-fly-to rows.

**9. Empty / error states**
- Distinct messages instead of a silent blank grid: "No save loaded — Load .sav…",
  `Model::error` text on parse failure, and "0 vehicles / stations found" when filters or
  an empty save yield nothing. Centred in the canvas.

**10. Persistence**
- `SaveSession()` already persists camera; extend to `sel_*`, `search`, `type_mask`,
  `sel_item`, `layers`, `color_by_item` so reopening the tool restores the working view.

**11. Keyboard / accessibility**
- `Esc` deselect / clear item query; `Up`/`Down` move selection through the filtered list
  (with fly-to on change optional); `F` = fit all. Document shortcuts in a small "?" popover.

### Deferred to v2 (noted, not built in v1)
- **Change-highlighting on autosave refresh:** diff new `Model` vs previous by id and pulse
  moved vehicles / new stations. Needs retained previous-model state — clean follow-up once
  the live-refresh path is proven.
- **Screenshot / export** of the current canvas view.
- **Throughput estimates** (items/min per route) — requires round-trip timing/modeling; out
  of scope for a read-only viewer.

### Where it slots into the task list
Tasks 1–5 unchanged. Task 4 gains the **derived indices**. Task 6 absorbs features 1–11 as
the concrete UI spec (selection/highlight, filters, layers, camera, panels). Build wiring
(task 7) is unaffected — no new translation units required, though a tiny `color_util`
helper (item-id → stable ImU32) may live in `utils`.

---

## Files

**New**
- `ficsit-companion/tools/sav_import/` — discovery dump script (throwaway) + `wrapper.js` `logistics` additions
- `ficsit-companion/include/vehicle_map.hpp`, `src/vehicle_map.cpp` — logistics data model + JSON parse
- `ficsit-companion/include/vehicle_map_app.hpp`, `src/vehicle_map_app.cpp` — the new `BaseApp` tool
- `ficsit-companion/include/sav_runner.hpp`, `src/sav_runner.cpp` — extracted subprocess runner
- `assets/` (or `icons/`) world-map background PNG
- `docs/save_game.md` — new vehicle/path topology section

**Modified**
- `ficsit-companion/src/main.cpp` — top tab switcher; instantiate both apps; render active
- `ficsit-companion/src/production_app.cpp` — call extracted `RunSavWrapper` (behavior unchanged)
- `ficsit-companion/CMakeLists.txt` — new sources + map asset

**Reused as-is**
- `save_watcher.hpp/cpp` (standalone), `LoadTextureFromFile` / `IsFuelItem` (`utils`),
  `Data::Items()` (global), `tools/sav_import/wrapper.js` parse pass.

---

## Verification (end-to-end)

1. **Discovery:** run the dump script on `example_save.sav`; confirm stations, 19
   vehicles, and path waypoints are extractable and that station↔vehicle↔path ids link up.
2. **Build:** `mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -S .. -B . &&
   cmake --build . --config Release` (Windows). Confirm `tools/sav_import` + map PNG copy
   to the runtime dir.
3. **Switcher:** launch app; tab between "Production Planner" and "Vehicle Map"; confirm
   the planner is unchanged.
4. **Load:** in Vehicle Map, "Load .sav…" → `example_save.sav`; confirm stations,
   vehicles, and path polylines render on the world map at plausible positions.
5. **Interaction:** click a station → its connected vehicles highlight + list in the side
   panel; click a vehicle → its path highlights and the stations it serves are shown.
6. **Usability:**
   - search filters both list and map; type/item filters hide non-matching entities.
   - item-centric query (pick an item) dims everything not in that item's network.
   - double-click a list row or marker flies the camera to it; `Esc` clears selection.
   - layer toggles hide/show roads/rails/vehicles/stations/labels; legend matches.
   - overlapping station+parked-vehicle markers fan out and are each clickable.
   - "Fit all" frames every visible entity; coordinate readout tracks the cursor.
   - "Issues" panel lists any orphan stations / pathless vehicles and flies to them.
   - empty/error states render a message, not a blank grid.
   - reopen the tool → camera, selection, and filters are restored.
7. **Live refresh:** enable watch on the SaveGames dir; trigger an in-game autosave (or
   copy a newer `.sav` in); confirm the map auto-updates.
8. Use `/run` to launch and screenshot the Vehicle Map with the sample save.

---

## Open risks / notes
- **Vehicle↔path↔station linkage is the main unknown** — gated behind the discovery step
  (task 2) before importer code is written.
- **Map calibration:** world→pixel constants must be tuned to the chosen PNG; the
  `LoadTextureFromFile` placeholder fallback means a missing/uncalibrated image degrades
  to a usable grid rather than breaking.
- **Web (Emscripten) parity** is secondary: switcher + data model are portable, but the
  desktop `_popen` runner is `#ifdef`-guarded; web uses the existing `web_loader.js` path.
  Target desktop first.
- **Render scale:** 627 paths + 489 waypoint nodes + 19 vehicles must stay smooth — the
  Usability plan addresses this with offscreen culling, zoom-dependent label hiding, and
  polyline decimation. Validate frame time on the sample save before adding more layers.
- **Usability is mostly view/derived state**, not new I/O — the main new persisted state is
  the extra `SaveSession()` fields, and the main new model state is the per-load indices
  (id→index, item→entities, orphans) built once after parse.
