# Windows build onboarding — design

**Date:** 2026-06-11
**Status:** Approved, pending implementation

## Goal

Lower the barrier for contributors who are not daily C++ developers to compile
ficsit-companion on Windows. The dependency story is already solved (CMake
`FetchContent` pulls SDL2, ImGui, ImGui Node Editor, nlohmann/json, Catch2 with
zero manual install). The remaining friction is **toolchain setup and knowing
the correct CMake invocation**, not dependencies.

This change covers build-easing only. Distribution improvements (CPack,
tag-triggered releases, winget/Scoop, code signing) are explicitly deferred.

## Scope

Three new/updated artifacts. No changes to existing CMake build logic, CI
workflows, or release workflow.

1. `CMakePresets.json` (new, repo root)
2. `build.ps1` (new, repo root)
3. README "Building" section (updated)

## 1. CMakePresets.json

Two configure presets plus matching build presets.

### `windows` configure preset
- Generator: **Visual Studio 17 2022**. Chosen over Ninja because it is what VS
  users get for free and requires no separate Ninja install; the target
  audience is newcomers who have Visual Studio installed.
- Cache variables:
  - `CMAKE_BUILD_TYPE=Release`
  - `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`
- Binary directory: `build/`

Rationale for `MultiThreaded`: it statically links the MSVC runtime, matching
what CI ships (`.github/workflows/build.yaml`). Today the README's plain
`cmake` command omits this flag and produces a dynamically-linked exe that
differs from the released binary. Baking it into the preset makes local builds
match releases.

### `emscripten` configure preset
- Cache variable `CMAKE_TOOLCHAIN_FILE` =
  `$env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`
- `CMAKE_BUILD_TYPE=Release`
- Requires the user to have run `emsdk activate` first so the `EMSDK`
  environment variable is set. Documented in the README.

### Build presets
- `windows` build preset → references the `windows` configure preset,
  `configuration: Release`.
- `emscripten` build preset → references the `emscripten` configure preset.

### Resulting workflows
- **Visual Studio:** `File → Open Folder` on the repo auto-detects the presets,
  configures, and builds. No command line.
- **VS Code + CMake Tools:** presets appear in the dropdown.
- **CLI:** `cmake --preset windows && cmake --build --preset windows`.

## 2. build.ps1

A detect-and-instruct one-shot script at the repo root. Non-intrusive: no
auto-install, no admin elevation, no surprise side effects.

Steps:
1. Check for `cmake` on PATH. If missing, print
   `winget install Kitware.CMake` and the VS Build Tools winget command, then
   exit non-zero.
2. Check for a C++ compiler: probe for Visual Studio via `vswhere` (or fall back
   to detecting `cl` on PATH). If missing, print the VS Build Tools /
   "Desktop development with C++" workload instructions and exit non-zero.
3. Otherwise run `cmake --preset windows` then
   `cmake --build --preset windows`, and print the path where the resulting
   `.exe` landed.

Intended to be double-click-friendly or run as `./build.ps1`.

## 3. README "Building" section update

Add a **Windows prerequisites** subsection above the existing build block:
- Visual Studio 2022 with the "Desktop development with C++" workload (bundles
  CMake), or the winget one-liners as an alternative.
- Three documented paths, easiest first:
  1. **Open Folder in Visual Studio** (no CLI).
  2. **`./build.ps1`** (one command).
  3. **Manual preset CLI** (`cmake --preset windows && cmake --build --preset windows`).
- A one-line note that **Node.js is only required for the optional
  `tools/sav_import` save-import feature**, not for building or running the core
  app.

Keep the existing cross-platform and web build instructions; update the web
instructions to mention the `emscripten` preset.

## Out of scope (deferred)

- CPack (NSIS installer + zip)
- Tag-triggered automatic releases (`release.yaml` is currently manual)
- winget / Scoop manifests
- Code signing
- Linux / macOS presets (presets cover Windows + Web only)

## Success criteria

- A contributor with Visual Studio 2022 installed can open the repo folder in VS
  and build the desktop app with no command line and no manual dependency steps.
- `./build.ps1` produces a working desktop `.exe` on a correctly-provisioned
  machine, and prints actionable guidance when CMake or a compiler is missing.
- The README clearly distinguishes the three build paths and clarifies that
  Node.js is optional.
- No regression to existing CI, release workflow, or CMake build logic.
