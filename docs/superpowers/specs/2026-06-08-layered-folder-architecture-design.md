# Layered Folder Architecture (domain / infra / app) — Design

**Date:** 2026-06-08
**Status:** Approved (pending spec review)

## Problem

After the ProductionApp DI decomposition, the codebase has a clean *logical*
architecture — a `fc-core` static library (pure logic + adapters) and the
`ficsit-companion` executable (ImGui presentation), with ports/adapters
(`IFileStore`, `IEditorBackend`) and a composition root (`ProductionApp`).

But all 25+ translation units sit **flat** in `ficsit-companion/src/` and
`ficsit-companion/include/`. The layering is real and build-enforced, yet
invisible when browsing the tree, and nothing makes a domain→infra dependency
violation obvious. The mess is *organizational/packaging*, not conceptual.

MVC was considered and rejected: Dear ImGui is immediate-mode (no retained View
to observe the Model), so the MVC triangle has nothing to attach to. A layered /
ports-and-adapters structure is the natural fit and already exists implicitly.

## Goal

Make the existing layers explicit by organizing files into `domain/`, `infra/`,
and `app/` folders (mirrored under both `src/` and `include/`), and enforce the
domain↔infra boundary through **layer-qualified includes**. **Pure move —
observable behavior must not change.**

## Layer definitions & file mapping

Both `src/` and `include/` receive the same three subfolders.

### `domain/` — pure logic, zero UI/IO dependencies
`node` · `pin` · `link` · `recipe` · `building` · `fractional_number` ·
`game_data` · `graph_model` · `rate_solver` · `graph_item_resolve` ·
`vehicle_map` · `json`

### `infra/` — ports + adapters, depends only on domain
`file_store` · `editor_backend` · `settings_store` · `session_serializer` ·
`sav_import` · `sav_import_service` · `sav_runner` · `save_watcher`

### `app/` — ImGui presentation, depends on domain + infra
`base_app` · `production_app` · `vehicle_map_app` · `utils` · `main`

### Special cases
- `stb_image.h` (third-party) → `include/third_party/` — kept out of the layer
  folders since it is not project code.
- `tests/` — unchanged. Tests are a *consumer* of `fc-core`, not a layer.
- `tools/sav_import/` — unchanged (external Node tooling).

## Dependency rule

```
app  ──►  infra  ──►  domain
 │                       ▲
 └───────────────────────┘   (app may also use domain directly)
```

- `domain/` includes only other `domain/` headers (and std / third-party).
- `infra/` includes `domain/` + `infra/`.
- `app/` includes any layer.
- A `domain/` file containing `#include "infra/..."` or `#include "app/..."`
  is a visible violation.

The `fc-core` (lib) vs `ficsit-companion` (exe) CMake split already enforces
*app ↛ domain/infra* at link time. Layer-qualified includes add enforcement
visibility for the *domain ↛ infra* boundary, which CMake does not separate
(both live in `fc-core`).

## Include strategy (Option A — layer-qualified)

Every cross-file include is rewritten to carry its layer prefix:

```cpp
#include "domain/node.hpp"
#include "infra/file_store.hpp"
#include "app/production_app.hpp"
```

`target_include_directories` keeps the root at `include/` (NOT the subfolders),
so the layer prefix is **mandatory** — a flat `#include "node.hpp"` will fail to
compile, preventing regression to the old style.

Trade-off accepted: this touches every `#include "..."` line across the
codebase (higher churn) in exchange for self-documenting include sites and
visible boundary violations.

## CMake changes (`ficsit-companion/CMakeLists.txt`)

1. Add `domain/`, `infra/`, `app/` path prefixes to the source lists.
2. Split `CORE_SOURCE_FILES` into `DOMAIN_SOURCE_FILES` + `INFRA_SOURCE_FILES`
   (both feed `add_library(fc-core ...)`) so the CMake lists themselves document
   the split.
3. Update the `app/` prefixes in `APP_SOURCE_FILES`, including the
   `src/app/utils.cpp` reference duplicated into the `fc-tests` target.
4. `HEADER_FILES` glob / list updated for the new include subfolders.
5. `source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} ...)` so the layer folders are
   reflected in the generated Visual Studio solution.
6. `target_include_directories` for `fc-core`, `ficsit-companion`, and
   `fc-tests` continue to point at `include/` root only.

## Migration mechanics

- **`git mv` per file** to preserve blame/history.
- **No git commits** — the user manages git (standing project rule).
- Move **one layer at a time**, in dependency order (`domain/` → `infra/` →
  `app/`). After each layer: fix that layer's includes + CMake, then **configure
  and build from the repo ROOT**:

  ```
  cmake -S . -B build
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```

  Three build/test checkpoints make any breakage easy to bisect.
- Pure move: no symbol, signature, or logic changes.

## Out of scope

- Further splitting `production_app.cpp` (~3,600 lines) into smaller files.
- Any behavior, logic, or signature change.
- CI workflow changes.
- Feature-based grouping (explicitly rejected in favor of layer-based).

## Risks & mitigations

- **Broken includes after a move** — mitigated by per-layer migration + build
  checkpoint after each layer; mandatory layer prefix surfaces any missed edit
  as a compile error immediately.
- **Emscripten/web build drift** — the `#ifdef __EMSCRIPTEN__` paths live inside
  moved files unchanged; verify a web configure step after the app layer if a
  web build is in use.
- **VS solution churn** — `source_group(TREE ...)` keeps the IDE view aligned
  with the new folders.
