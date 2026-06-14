# ProductionApp Decomposition & Unit Testing — Design

**Date:** 2026-06-08
**Status:** Approved (pending spec review)

## Problem

`ficsit-companion/src/production_app.cpp` is ~5082 lines and concentrates many
unrelated responsibilities in a single class: file persistence, JSON
serialization, rate propagation, graph mutation, settings, save-file import
orchestration, and ~2500 lines of immediate-mode ImGui rendering. The file is
hard to reason about, and none of the logic is unit-testable because everything
is private state on one monolithic class with no test framework in the repo.

## Goal

Split the file by responsibility using dependency injection so the pure-logic
concerns become independently testable, and add Catch2 unit tests for each
extracted module. **This is a pure refactor — observable behavior must not
change.**

## Scope decisions (locked during brainstorming)

- **Approach:** Pragmatic extraction. Extract testable pure-logic concerns into
  injectable classes; leave rendering coupled to `ProductionApp` (immediate-mode
  UI cannot be meaningfully unit-tested without a headless harness — out of
  scope).
- **Test framework:** Catch2, fetched via CMake `FetchContent` (consistent with
  SDL2/ImGui/json).
- **DI mechanism:** Constructor injection via abstract base classes, passed as
  references / `std::unique_ptr`.

## Architecture

### Build restructure

The project currently compiles every `.cpp` into a single executable. To make
logic linkable into a test binary, introduce a **static core library**:

```
fc-core (STATIC lib)   ← all pure logic + extracted modules
   ├── ficsit-companion   (executable: app + rendering + main, links fc-core)
   └── fc-tests           (executable: Catch2, links fc-core)
```

`fc-core` contains the existing pure files (`node`, `pin`, `link`, `recipe`,
`building`, `fractional_number`, `json`, `game_data`, `sav_import`,
`sav_runner`, `save_watcher`, `utils`, `vehicle_map`) plus the new modules
below. The ImGui sources and rendering-only translation units
(`production_app.cpp`, `vehicle_map_app.cpp`, `base_app.cpp`, `main.cpp`) stay
in the `ficsit-companion` executable target.

> Note: `fc-core` still transitively depends on ImGui headers because
> `node.hpp`/`pin.hpp` use `ax::NodeEditor::*Id` types and `ImVec2`. These are
> header/type-only dependencies (no live context needed), so the core lib and
> tests compile against ImGui include folders without creating a window.

### DI seams (two interfaces)

1. **`IFileStore`** — abstracts text file read/write/remove, replacing the
   file-static `SaveFile` / `LoadFile` / `RemoveFile` `#ifdef __EMSCRIPTEN__`
   block.
   - `std::optional<std::string> Load(const std::string& path) const`
   - `void Save(const std::string& path, const std::string& content)`
   - `void Remove(const std::string& path)`
   - Implementations: `DiskFileStore` (std::fstream/std::filesystem),
     `WebFileStore` (EM_ASM localStorage), `InMemoryFileStore` (test fake,
     backed by `std::map`).

2. **`IEditorBackend`** — abstracts the `ax::NodeEditor::*` free functions used
   *outside* rendering, so graph mutation and serialization can run headless.
   - `void DeleteNode(ax::NodeEditor::NodeId)`
   - `void DeleteLink(ax::NodeEditor::LinkId)`
   - `void SetNodePosition(ax::NodeEditor::NodeId, ImVec2)`
   - `ImVec2 GetNodePosition(ax::NodeEditor::NodeId)`
   - (plus any other `ax::NodeEditor` calls discovered in the extracted fns;
     the exact method set is finalized during implementation by auditing the
     moved code.)
   - Implementations: `NodeEditorBackend` (forwards to real `ax::NodeEditor`),
     `FakeEditorBackend` (records calls / stores positions in a map for tests).

### Extracted modules

| Module (files) | Responsibility | Moved from `ProductionApp` | DI deps |
|---|---|---|---|
| `file_store.hpp/cpp` | `IFileStore` + Disk/Web/InMemory impls | `SaveFile`/`LoadFile`/`RemoveFile` | — |
| `editor_backend.hpp/cpp` | `IEditorBackend` + real/fake impls | scattered `ax::NodeEditor` calls | — |
| `rate_solver.hpp/cpp` | `RateSolver` — rate propagation | `UpdateNodesRate` (~805 lines) | `IEditorBackend` (link flow) |
| `graph_model.hpp/cpp` | `GraphModel` — owns `nodes`/`links`; `GetNextId`, `FindPin`, `CreateLink`, `DeleteLink`, `DeleteNode` | those 5 fns | `IEditorBackend` |
| `session_serializer.hpp/cpp` | `SessionSerializer` — graph ↔ JSON string | `Serialize` / `Deserialize` | `GraphModel`, `IEditorBackend` |
| `settings_store.hpp/cpp` | `Settings` struct (moved out) + JSON load/save | `Settings`, `LoadSettings`, `SaveSettings` | `IFileStore` |
| `sav_import_service.hpp/cpp` | `SavImportService` — import orchestration | `ImportSavFromJson`/`ImportSavFile`/`DrainPendingImports`/`RefreshDiscoveredWorlds` | `GraphModel`, `IFileStore` |

### `ProductionApp` after refactor

`ProductionApp` retains and owns:
- All rendering: `RenderImpl`, `RenderNodes`, `RenderLeftPanel`, `RenderLinks`,
  `DragLink`, `AddNewNode`, `RenderTooltips`, `RenderControlsPopup`,
  `RenderSavImportSection`, `CustomKeyControl`.
- Node operations: `GroupSelectedNodes`, `UngroupSelectedNode`,
  `DuplicateSelectedNodes`, `NudgeNodes`, `PullNodesPosition`.
- Navigation: `FocusNextRecipe`, `FocusNextItem`, `FocusNextSomersloop`.

It becomes the **composition root**: in its constructor it instantiates the
concrete `DiskFileStore`/`WebFileStore` (chosen by `#ifdef __EMSCRIPTEN__`) and
`NodeEditorBackend`, and injects them into the `GraphModel`, `RateSolver`,
`SessionSerializer`, `SettingsStore`, and `SavImportService` instances it owns.
The graph (`nodes`/`links`) now lives in `GraphModel`; rendering accesses it
through the owned `GraphModel`.

### Data flow (unchanged in behavior)

```
user edits → ProductionApp render handlers
   → GraphModel.CreateLink/DeleteLink/DeleteNode (via IEditorBackend)
   → RateSolver.Solve(graph, constraint_pin, value)   (was UpdateNodesRate)
   → on change: SessionSerializer.Serialize → IFileStore.Save
settings toggles → SettingsStore.Save → IFileStore.Save
sav import → SavImportService → GraphModel (append group node)
```

## Testing strategy

New `fc-tests` Catch2 executable, one test file per module:

- **`test_rate_solver.cpp`** *(highest value)* — build small graphs
  (merger/splitter/craft) from **fabricated** `Recipe`/`Item` structs (plain
  structs, no `Data::LoadData` needed), drive `RateSolver.Solve`, assert exact
  `FractionalNumber` rates on pins. Covers balanced splitter, merger sum, craft
  ratio, locked-pin constraints, error/overflow cases.
- **`test_session_serializer.cpp`** — round-trip a graph → JSON → graph and
  assert structural equality; assert version-mismatch handling.
- **`test_settings_store.cpp`** — round-trip `Settings` through
  `InMemoryFileStore`; assert defaults when file absent.
- **`test_graph_model.cpp`** — `CreateLink`/`DeleteLink`/`DeleteNode` invariants
  (one link per pin, dangling link cleanup) verified via `FakeEditorBackend`.
- **`test_sav_import_service.cpp`** — world discovery from a fake folder listing
  and JSON→graph append, using `InMemoryFileStore`.

Registered with `ctest`. Run via `ctest` in the build dir (or the test
executable directly).

## Out of scope

- Rendering decoupling (immediate-mode UI; cannot be unit-tested without a
  headless harness).
- Splitting the ~2500 lines of rendering into separate files (readability-only;
  not requested).
- CI workflow changes.
- Any behavior change.

## Risks & mitigations

- **Behavior regression** — mitigate by moving code verbatim (not rewriting),
  building after each module extraction, and manually launching the app to
  confirm load/edit/save/import still work.
- **Hidden coupling discovered mid-extraction** — the `IEditorBackend` method
  set is finalized by auditing moved code; add methods as needed.
- **Web (Emscripten) build** — `WebFileStore` preserves the existing EM_ASM
  paths; the `#ifdef` moves from free functions into the composition root.
