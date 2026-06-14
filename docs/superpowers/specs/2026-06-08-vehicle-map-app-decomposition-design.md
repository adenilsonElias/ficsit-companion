# VehicleMapApp Decomposition & Unit Testing — Design

**Date:** 2026-06-08
**Status:** Approved (pending spec review)

## Problem

`ficsit-companion/src/app/vehicle_map_app.cpp` is ~1,128 lines and concentrates
six unrelated responsibilities in one ImGui class: camera/transform math,
filter/highlight queries, session serialization, save-file import + world
discovery, file I/O, and ~675 lines of immediate-mode rendering. Three of those
concerns are pure logic that is currently untestable only because it is tangled
into the rendering class.

This continues the architecture established by the ProductionApp decomposition
and the domain/infra/app layering: pull testable pure-logic cores into
injectable `fc-core`-linked modules with Catch2 tests, and leave rendering in
the app as composition root. **Pure refactor — observable behavior must not
change.**

## Goal

1. Extract four modules from `VehicleMapApp`, each with a Catch2 test file.
2. Wire the existing `fc-tests` suite into CI (it currently never runs).

## Modules

### 1. `domain/vehicle_map_camera.{hpp,cpp}` — `VehicleMapCamera`

Owns `pan`, `zoom`, canvas geometry (`canvas_p0`, `canvas_sz`), and the fly-to
animation state. `kDefaultZoom = 0.02f` moves here.

API:
- `ImVec2 WorldToScreen(const ImVec2& world) const`
- `ImVec2 ScreenToWorld(const ImVec2& screen) const`
- `void SetCanvas(const ImVec2& p0, const ImVec2& size)`
- `void FitAll(const ImVec2& world_min, const ImVec2& world_max, bool has_bounds)`
- `void PanBy(const ImVec2& delta)`
- `void ZoomAbout(const ImVec2& screen_anchor, float wheel)`
- `void StartFlyTo(const ImVec2& world_pos, float target_zoom)`
- `void UpdateFlyTo(float dt)` — **`dt` injected** in place of
  `ImGui::GetIO().DeltaTime`; the app passes `ImGui::GetIO().DeltaTime`.
- accessors/mutators for `pan`/`zoom`/`flyto_active` (used by gather/scatter and
  by interaction code that disables fly-to on manual input).

`VehicleMapApp` calls these from `RenderCanvas`; the drag-pan and zoom-about-
cursor blocks become `PanBy` / `ZoomAbout`.

**Tests (`test_vehicle_map_camera.cpp`):**
- `ScreenToWorld(WorldToScreen(p)) == p` for several pan/zoom/canvas configs.
- `ZoomAbout` keeps the world point under the anchor fixed.
- `FitAll` centers given bounds within the canvas and clamps non-finite/≤0 zoom
  to `kDefaultZoom`; no-op when `!has_bounds`.
- `UpdateFlyTo` interpolates with smoothstep and lands exactly on the target
  pan/zoom once accumulated `t ≥ 1`, then clears `flyto_active`.

### 2. `domain/vehicle_map_query.{hpp,cpp}` — filtering + highlight

Free functions in namespace `VehicleMapQuery`, plus the `Highlight` struct moved
out of `VehicleMapApp`:
- `bool ContainsCI(const std::string& haystack, const std::string& needle)`
- `bool StationPassesFilter(const VehicleMap::Station&, const std::string& search)`
- `bool VehiclePassesFilter(const VehicleMap::Vehicle&, const std::string& search, unsigned int type_mask)`
- `struct Highlight { bool active; std::unordered_set<std::string> stations, vehicles; std::vector<std::vector<ImVec2>> routes; };`
- `Highlight ComputeHighlight(const VehicleMap::Model&, const std::string& sel_vehicle, const std::string& sel_station, const std::string& sel_item)`

`ToLower` / `TypeBit` helpers move here (or stay file-local where still needed by
rendering). `VehicleColor` / `HashColor` / `WithAlpha` are ImGui-color helpers
that stay in the rendering TU (not pure logic).

**Tests (`test_vehicle_map_query.cpp`):**
- `ContainsCI` case-insensitivity and empty-needle = match-all.
- Station filter matches on name and on item names; vehicle filter respects
  `type_mask` and matches on name / type name.
- `ComputeHighlight` for a selected vehicle (its stations + route polyline), a
  selected station (its vehicles + their routes), and an item query (all
  stations/vehicles carrying it).

### 3. `infra/vehicle_map_session.{hpp,cpp}` — `VehicleMapSession`

POD DTO of all persisted state + JSON (de)serialization, mirroring
`SettingsStore` / `SessionSerializer`:

```cpp
struct VehicleMapSession {
    std::string node_executable_path, sav_watch_dir, sav_watch_world;
    bool sav_watch_enabled = false;
    std::string last_sav_path;
    ImVec2 pan{0,0};
    float zoom = 0.02f;             // kDefaultZoom
    std::string sel_station, sel_vehicle, sel_item, search;
    unsigned int type_mask = 0xFFFFFFFFu;
    struct Layers { bool roads=true, rails=true, vehicles=true, stations=true, labels=true; } layers;
    bool color_by_item = false;

    std::string Serialize() const;
    void Deserialize(const std::string& json);   // tolerant; unknown/missing keys keep defaults
};
```

The app **gathers** live state (camera `pan`/`zoom` + its own members) into a
`VehicleMapSession` on save, and **scatters** the DTO back into live state on
load. File I/O (`LoadSession`/`SaveSession`, the `saved/vehicle_map.json` path,
the `#ifdef __EMSCRIPTEN__` guards) stays in the app.

**Tests (`test_vehicle_map_session.cpp`):**
- DTO → `Serialize` → `Deserialize` round-trips every field.
- Empty/garbage JSON leaves defaults; partial JSON keeps defaults for absent
  keys.
- `zoom ≤ 0` in JSON is clamped to `kDefaultZoom`.

### 4. Import / world-discovery — reuse + one extraction

- `RefreshDiscoveredWorlds` is rewritten to enumerate `.sav` file stems from
  `sav_watch_dir` and call the existing, already-tested
  `DiscoverWorldNames(stems)` (drops the bespoke `find_last_of('_')` trimming;
  same approach ProductionApp uses at `production_app.cpp:3291`). Filesystem
  enumeration stays in the app.
- Extract the logistics-warning filter from `LoadSavFile` into a pure function
  next to `ParseLogisticsJson` in `domain/vehicle_map`:
  `std::vector<std::string> ExtractLogisticsWarnings(const std::string& json)`.

**Tests (`test_vehicle_map_import.cpp`):**
- `ExtractLogisticsWarnings` returns only warnings whose text contains
  "logistics"; returns empty on malformed JSON / missing `warnings` array.
- (World-name de-dupe/sort already covered by `test_sav_import_service.cpp`.)

## `VehicleMapApp` after refactor

Becomes the composition root: owns a `VehicleMapCamera`, calls `VehicleMapQuery`
free functions, uses a `VehicleMapSession` for persistence, and reuses
`DiscoverWorldNames` + `ExtractLogisticsWarnings`. Retains all `Render*`
methods, `SelectStation`/`SelectVehicle`/`ClearSelection` (selection state +
fly-to triggering), `LoadSavFile` orchestration, and the file-I/O wrappers.

## CI

Add a **Test** step to the `build-desktop` job in `.github/workflows/build.yaml`
(runs on ubuntu/windows/macos), after the Build step:

```yaml
- name: Test
  run: ctest --test-dir ${{ github.workspace }}/build -C Release --output-on-failure
```

`fc-tests` already builds with the default target (Catch2 is fetched via
FetchContent), so no extra build configuration is required. The web job is
unchanged (`fc-tests` is desktop-only).

## Build restructure

Four new `.cpp` files added to CMake: three to `fc-core`
(`DOMAIN_SOURCE_FILES`: `vehicle_map_camera`, `vehicle_map_query`;
`INFRA_SOURCE_FILES`: `vehicle_map_session`) and `ExtractLogisticsWarnings`
folded into the existing `domain/vehicle_map.cpp`. Four new test `.cpp` files
added to `TEST_SOURCE_FILES`. New headers added to `HEADER_FILES`.

## Testing strategy

New Catch2 files registered with `ctest`, one per module. Tests fabricate
`VehicleMap::Model`/`Station`/`Vehicle` as plain structs (no game data or ImGui
context needed); camera/query tests use only `ImVec2` math. Consistent with the
existing fc-tests fakes (`InMemoryFileStore`, `FakeEditorBackend`).

## Migration mechanics

- Move code **verbatim** (not rewritten); configure/build from repo ROOT
  (`cmake -S . -B build`); build + `ctest` after each module extraction.
- **No git commits** — the user manages git.
- Pure refactor: no behavior, signature, or rendering change beyond the moved
  call sites.

## Out of scope

- The ~675 lines of rendering body (immediate-mode; not unit-testable without a
  headless harness).
- The known `domain → app/utils.hpp` and `domain → infra/editor_backend.hpp`
  layer violations (tracked separately).
- Web (Emscripten) test execution.

## Risks & mitigations

- **Behavior regression** — verbatim moves, per-module build+test checkpoints,
  and manual app launch to confirm load/pan/zoom/select/persist still work.
- **State-ownership split (camera vs app)** — `pan`/`zoom` move into the camera;
  audit every read/write of those fields in the rendering code and route through
  the camera accessors. Build errors surface any missed reference.
- **Session gather/scatter drift** — the DTO field set must exactly match the
  old `Serialize`/`Deserialize`; the round-trip test plus a manual save/reload
  guard against dropped fields.
