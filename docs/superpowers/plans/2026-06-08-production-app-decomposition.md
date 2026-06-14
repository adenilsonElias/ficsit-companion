# ProductionApp Decomposition + Unit Tests — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the ~5082-line `production_app.cpp` into focused, dependency-injected modules in a `fc-core` static library, and add Catch2 unit tests for each extracted module — without changing any observable behavior.

**Architecture:** Pull pure-logic concerns (file I/O, settings, rate propagation, graph mutation, serialization, sav-import orchestration) out of `ProductionApp` into separate classes behind two DI interfaces (`IFileStore`, `IEditorBackend`). `ProductionApp` becomes the composition root + rendering host. Two risk-reduction techniques keep the ~2500 lines of ImGui rendering code untouched: (a) `ProductionApp` binds `nodes`/`links` as C++ reference members to the `GraphModel`'s vectors; (b) the methods rendering calls (`CreateLink`, `DeleteLink`, `DeleteNode`, `FindPin`, `UpdateNodesRate`, `GetNextId`, `Serialize`, etc.) survive as one-line delegating wrappers.

**Tech Stack:** C++17, CMake + FetchContent, Catch2 v3, Dear ImGui + imgui-node-editor (header/type-only dependency for core), nlohmann-free custom `Json`.

**Spec:** `docs/superpowers/specs/2026-06-08-production-app-decomposition-design.md`

---

## File structure (created / modified)

Created (all under `ficsit-companion/`, headers in `include/`, sources in `src/`, tests in `tests/`):

| File | Responsibility |
|---|---|
| `include/file_store.hpp`, `src/file_store.cpp` | `IFileStore` + `DiskFileStore`, `WebFileStore`, `InMemoryFileStore` |
| `include/editor_backend.hpp`, `src/editor_backend.cpp` | `IEditorBackend` + `NodeEditorBackend`, `FakeEditorBackend` |
| `include/rate_solver.hpp`, `src/rate_solver.cpp` | `RateSolver::Solve` (moved `UpdateNodesRate`) |
| `include/graph_model.hpp`, `src/graph_model.cpp` | `GraphModel` (owns `nodes`/`links`; `GetNextId`, `FindPin`, `CreateLink`, `DeleteLink`, `DeleteNode`) |
| `include/session_serializer.hpp`, `src/session_serializer.cpp` | `SessionSerializer` (moved `Serialize`/`Deserialize`) |
| `include/settings_store.hpp`, `src/settings_store.cpp` | `Settings` struct + `SettingsStore` (moved `LoadSettings`/`SaveSettings`) |
| `include/sav_import_service.hpp`, `src/sav_import_service.cpp` | `SavImportService` (moved import orchestration) + pure `DeriveWorldName` |
| `tests/test_main.cpp` | Catch2 entry (or use provided main) |
| `tests/test_rate_solver.cpp`, `tests/test_session_serializer.cpp`, `tests/test_settings_store.cpp`, `tests/test_graph_model.cpp`, `tests/test_sav_import_service.cpp`, `tests/graph_test_helpers.hpp` | Unit tests + shared fixtures |
| `cmake/catch2.cmake` | FetchContent for Catch2 |

Modified:
- `ficsit-companion/CMakeLists.txt` — split into `fc-core` STATIC lib + `ficsit-companion` exe + `fc-tests` exe.
- `CMakeLists.txt` (root) — include `cmake/catch2.cmake`, `enable_testing()`.
- `ficsit-companion/include/production_app.hpp` / `src/production_app.cpp` — own the new modules, keep delegating wrappers, remove moved bodies.

---

## Task 1: Build infrastructure — `fc-core` library + Catch2 `fc-tests` target

**Files:**
- Modify: `ficsit-companion/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root)
- Create: `cmake/catch2.cmake`
- Create: `ficsit-companion/tests/test_main.cpp`
- Create: `ficsit-companion/tests/test_smoke.cpp`

- [ ] **Step 1: Inspect the root CMakeLists to find where subdirectories/cmake modules are included**

Run: `rg -n "add_subdirectory|include\(|FetchContent" CMakeLists.txt cmake/*.cmake`
Expected: shows how `cmake/imgui.cmake` etc. are pulled in. Mirror that style.

- [ ] **Step 2: Create `cmake/catch2.cmake`**

```cmake
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(Catch2)
```

- [ ] **Step 3: Wire Catch2 + testing into root `CMakeLists.txt`**

Add near the other `include(cmake/*.cmake)` lines (only when not building for web — Catch2 needs a native test runner):

```cmake
if (NOT DEFINED EMSCRIPTEN)
    enable_testing()
    include(cmake/catch2.cmake)
endif()
```

- [ ] **Step 4: Restructure `ficsit-companion/CMakeLists.txt` into a core lib + exe + tests**

Replace the single `add_executable` setup. Define which sources are "core" (linkable + testable) vs "app" (rendering/main). Core sources:

```cmake
set(CORE_SOURCE_FILES
    src/building.cpp
    src/fractional_number.cpp
    src/game_data.cpp
    src/json.cpp
    src/link.cpp
    src/node.cpp
    src/pin.cpp
    src/recipe.cpp
    src/sav_import.cpp
    src/sav_runner.cpp
    src/save_watcher.cpp
    src/utils.cpp
    src/vehicle_map.cpp
    # New extracted modules (added by later tasks):
    src/file_store.cpp
    src/editor_backend.cpp
    src/rate_solver.cpp
    src/graph_model.cpp
    src/session_serializer.cpp
    src/settings_store.cpp
    src/sav_import_service.cpp
)

add_library(fc-core STATIC ${CORE_SOURCE_FILES})
set_property(TARGET fc-core PROPERTY CXX_STANDARD 17)
target_include_directories(fc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(fc-core PUBLIC ${imgui_INCLUDE_FOLDERS})
```

> NOTE: `utils.cpp` may pull in OpenGL/texture code. If `fc-core` fails to link for tests because of that, move `utils.cpp` to the app target and have core depend only on the pure helpers it needs. Verify in Step 7 and adjust.

The app executable links core + the rendering/main sources + ImGui:

```cmake
set(APP_SOURCE_FILES
    ${imgui_SOURCE}
    src/base_app.cpp
    src/production_app.cpp
    src/vehicle_map_app.cpp
    src/main.cpp
)
source_group(ImGui FILES ${imgui_SOURCE})

add_executable(${PROJECT_NAME} ${HEADER_FILES} ${APP_SOURCE_FILES})
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_INCLUDE_FOLDERS})
target_link_libraries(${PROJECT_NAME} PRIVATE fc-core)
```

Keep all the existing desktop/emscripten `if (NOT DEFINED EMSCRIPTEN) ... else() ... endif()` blocks (OpenGL/SDL2 link, asset copy, install rules, emscripten flags) exactly as before, still targeting `${PROJECT_NAME}`. The ImGui sources must compile into the app (and into core's include path) as today; if core needs the ImGui object files at link time for tests, also link ImGui into `fc-core`:

```cmake
target_sources(fc-core PRIVATE ${imgui_SOURCE})
```

(Adjust in Step 7 based on actual link errors — prefer the smallest set that links.)

- [ ] **Step 5: Add the `fc-tests` executable at the end of `ficsit-companion/CMakeLists.txt`**

```cmake
if (NOT DEFINED EMSCRIPTEN AND TARGET Catch2::Catch2WithMain)
    set(TEST_SOURCE_FILES
        tests/test_smoke.cpp
        # New test files added by later tasks:
        # tests/test_settings_store.cpp
        # tests/test_rate_solver.cpp
        # tests/test_graph_model.cpp
        # tests/test_session_serializer.cpp
        # tests/test_sav_import_service.cpp
    )
    add_executable(fc-tests ${TEST_SOURCE_FILES})
    set_property(TARGET fc-tests PROPERTY CXX_STANDARD 17)
    target_link_libraries(fc-tests PRIVATE fc-core Catch2::Catch2WithMain)
    target_include_directories(fc-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    include(Catch)
    catch_discover_tests(fc-tests)
endif()
```

> Using `Catch2::Catch2WithMain` provides `main()`, so `tests/test_main.cpp` is not needed. Do NOT create `test_main.cpp`; remove it from the file list above. (Listed in file structure for completeness only.)

- [ ] **Step 6: Create the smoke test `ficsit-companion/tests/test_smoke.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("smoke: test harness links and runs", "[smoke]")
{
    REQUIRE(1 + 1 == 2);
}
```

- [ ] **Step 7: Configure and build everything**

Run:
```
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build --config Release
```
Expected: `fc-core`, `ficsit-companion`, and `fc-tests` all build. If `fc-core` fails to link due to OpenGL/SDL/texture symbols pulled in by `utils.cpp`/`game_data.cpp`, move the offending `.cpp` to `APP_SOURCE_FILES` and keep only genuinely pure files in core, then rebuild. Iterate until all three targets build.

- [ ] **Step 8: Run the smoke test**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: `test harness links and runs` PASSES (1 test passed).

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt cmake/catch2.cmake ficsit-companion/CMakeLists.txt ficsit-companion/tests/test_smoke.cpp
git commit -m "build: add fc-core static lib and Catch2 fc-tests target"
```

---

## Task 2: `IFileStore` abstraction

Extract the file-static `SaveFile`/`LoadFile`/`RemoveFile` block (`production_app.cpp:70-126`) into an injectable interface. `ProductionApp` will own an `IFileStore` and route all persistence through it.

**Files:**
- Create: `ficsit-companion/include/file_store.hpp`
- Create: `ficsit-companion/src/file_store.cpp`
- Create: `ficsit-companion/tests/test_file_store.cpp`
- Modify: `ficsit-companion/CMakeLists.txt` (uncomment `tests/test_file_store.cpp`)

- [ ] **Step 1: Create `include/file_store.hpp`**

```cpp
#pragma once

#include <map>
#include <optional>
#include <string>

/// @brief Abstraction over text file persistence so logic is testable and the
/// desktop/web (localStorage) split lives behind one seam.
class IFileStore
{
public:
    virtual ~IFileStore() = default;
    /// @return file content, or std::nullopt when the path does not exist
    virtual std::optional<std::string> Load(const std::string& path) const = 0;
    virtual void Save(const std::string& path, const std::string& content) = 0;
    virtual void Remove(const std::string& path) = 0;
};

/// @brief Desktop implementation backed by std::filesystem / fstream.
class DiskFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
};

#if defined(__EMSCRIPTEN__)
/// @brief Web implementation backed by browser localStorage (EM_ASM).
class WebFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
};
#endif

/// @brief In-memory fake for unit tests.
class InMemoryFileStore : public IFileStore
{
public:
    std::optional<std::string> Load(const std::string& path) const override;
    void Save(const std::string& path, const std::string& content) override;
    void Remove(const std::string& path) override;
    std::map<std::string, std::string> files;
};
```

- [ ] **Step 2: Write the failing test `tests/test_file_store.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "file_store.hpp"

TEST_CASE("InMemoryFileStore round-trips content", "[file_store]")
{
    InMemoryFileStore store;
    REQUIRE_FALSE(store.Load("a.txt").has_value());

    store.Save("a.txt", "hello");
    auto loaded = store.Load("a.txt");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == "hello");

    store.Remove("a.txt");
    REQUIRE_FALSE(store.Load("a.txt").has_value());
}
```

- [ ] **Step 3: Add `tests/test_file_store.cpp` to `TEST_SOURCE_FILES` in `ficsit-companion/CMakeLists.txt`, then build to verify it FAILS to link**

Run: `cmake --build build --config Release`
Expected: link error — `InMemoryFileStore` symbols undefined (no `file_store.cpp` yet). This confirms the test targets real code.

- [ ] **Step 4: Implement `src/file_store.cpp`**

Move the bodies of `SaveFile`/`LoadFile`/`RemoveFile` from `production_app.cpp:70-126` verbatim into the matching methods. `InMemoryFileStore`:

```cpp
#include "file_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

std::optional<std::string> DiskFileStore::Load(const std::string& path) const
{
    if (std::filesystem::exists(path))
    {
        std::ifstream f(path, std::ios::in);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return std::nullopt;
}

void DiskFileStore::Save(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::out);
    f << content;
    f.close();
}

void DiskFileStore::Remove(const std::string& path)
{
    if (std::filesystem::exists(path))
    {
        std::filesystem::remove(path);
    }
}

#if defined(__EMSCRIPTEN__)
std::optional<std::string> WebFileStore::Load(const std::string& path) const
{
    char* content_raw = static_cast<char*>(EM_ASM_PTR({
        var str = localStorage.getItem(UTF8ToString($0)) || "";
        var length = lengthBytesUTF8(str) + 1;
        var str_wasm = _malloc(length);
        stringToUTF8(str, str_wasm, length);
        return str_wasm;
    }, path.c_str()));
    const std::string content(content_raw);
    free(static_cast<void*>(content_raw));
    return content.empty() ? std::nullopt : std::optional<std::string>(content);
}

void WebFileStore::Save(const std::string& path, const std::string& content)
{
    EM_ASM({ localStorage.setItem(UTF8ToString($0), UTF8ToString($1)); }, path.c_str(), content.c_str());
}

void WebFileStore::Remove(const std::string& path)
{
    EM_ASM({ localStorage.removeItem(UTF8ToString($0)); }, path.c_str());
}
#endif

std::optional<std::string> InMemoryFileStore::Load(const std::string& path) const
{
    auto it = files.find(path);
    if (it == files.end()) return std::nullopt;
    return it->second;
}

void InMemoryFileStore::Save(const std::string& path, const std::string& content) { files[path] = content; }

void InMemoryFileStore::Remove(const std::string& path) { files.erase(path); }
```

- [ ] **Step 5: Rewire `ProductionApp` to use `IFileStore` instead of the free functions**

In `production_app.hpp`: add `#include "file_store.hpp"` and a member `std::unique_ptr<IFileStore> file_store;`.

In `production_app.cpp`: delete the `SaveFile`/`LoadFile`/`RemoveFile` static functions (lines 70-126). In the constructor (before `LoadSettings()`), initialize the store:

```cpp
#if defined(__EMSCRIPTEN__)
    file_store = std::make_unique<WebFileStore>();
#else
    file_store = std::make_unique<DiskFileStore>();
#endif
```

Replace every `SaveFile(x, y)` → `file_store->Save(x, y)`, `LoadFile(x)` → `file_store->Load(x)`, `RemoveFile(x)` → `file_store->Remove(x)` in `production_app.cpp`. Find them with: `rg -n "SaveFile|LoadFile|RemoveFile" ficsit-companion/src/production_app.cpp`.

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: all targets build; `[file_store]` test PASSES; smoke still passes.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/file_store.hpp ficsit-companion/src/file_store.cpp ficsit-companion/tests/test_file_store.cpp ficsit-companion/CMakeLists.txt ficsit-companion/include/production_app.hpp ficsit-companion/src/production_app.cpp
git commit -m "refactor: extract IFileStore from ProductionApp"
```

---

## Task 3: `SettingsStore` + `Settings` struct

Move the `Settings` struct out of `ProductionApp` and extract `LoadSettings`/`SaveSettings` (`production_app.cpp:194-297`) into a `SettingsStore` that takes an `IFileStore&`.

**Files:**
- Create: `ficsit-companion/include/settings_store.hpp`
- Create: `ficsit-companion/src/settings_store.cpp`
- Create: `ficsit-companion/tests/test_settings_store.cpp`
- Modify: `production_app.hpp` / `production_app.cpp`, `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create `include/settings_store.hpp`**

Move the `Settings` struct verbatim from `production_app.hpp:155-181` into this header (rename `struct Settings {...} settings;` to a standalone `struct Settings { ... };`). Keep includes it needs (`<map>`, `<string>`, `"sav_import.hpp"`, `struct Recipe;`).

```cpp
#pragma once

#include <map>
#include <string>

#include "sav_import.hpp"

struct Recipe;
class IFileStore;

struct Settings {
    bool show_spoilers = false;
    bool show_somersloop = false;
    std::map<const Recipe*, bool> unlocked_alts = {};
    bool power_equal_clocks = true;
    bool show_build_progress = false;
    bool left_panel_folded = false;
    std::string sav_watch_dir;
    std::string sav_watch_world;
    bool sav_watch_enabled = false;
    std::string node_executable_path;
    SavImport::LayoutMode sav_import_layout_mode = SavImport::LayoutMode::Compact;
    float sav_import_world_spacing_scale = SavImport::kPositionScale;
    bool sav_import_connect_vehicle_routes = false;
};

/// @brief Loads/saves Settings as JSON through an IFileStore. Pure of UI.
class SettingsStore
{
public:
    SettingsStore(IFileStore& file_store, std::string settings_path);

    /// @brief Populate `out` from the settings file. `alt_recipes` is the list
    /// of recipes whose alternate-unlock flags should be read (typically
    /// Data::Recipes()). Writes defaults back to disk if the file was absent.
    void Load(Settings& out, const std::vector<const Recipe*>& alt_recipes);
    void Save(const Settings& settings) const;

private:
    IFileStore& file_store;
    std::string settings_path;
};
```

- [ ] **Step 2: Write the failing test `tests/test_settings_store.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "file_store.hpp"
#include "settings_store.hpp"

TEST_CASE("SettingsStore round-trips scalar settings", "[settings]")
{
    InMemoryFileStore fs;
    SettingsStore store(fs, "settings.json");

    Settings s;
    s.show_somersloop = true;
    s.power_equal_clocks = false;
    s.left_panel_folded = true;
    s.sav_watch_dir = "C:/saves";
    s.sav_watch_world = "MyWorld";
    s.sav_import_world_spacing_scale = 2.5f;
    store.Save(s);

    Settings loaded;
    store.Load(loaded, {});
    REQUIRE(loaded.show_somersloop == true);
    REQUIRE(loaded.power_equal_clocks == false);
    REQUIRE(loaded.left_panel_folded == true);
    REQUIRE(loaded.sav_watch_dir == "C:/saves");
    REQUIRE(loaded.sav_watch_world == "MyWorld");
    REQUIRE(loaded.sav_import_world_spacing_scale == 2.5f);
}

TEST_CASE("SettingsStore Load applies defaults when file absent", "[settings]")
{
    InMemoryFileStore fs;
    SettingsStore store(fs, "settings.json");
    Settings loaded;
    store.Load(loaded, {});
    REQUIRE(loaded.power_equal_clocks == false); // JSON-absent default per LoadSettings logic
    REQUIRE(loaded.sav_import_world_spacing_scale == SavImport::kPositionScale);
}
```

> NOTE: The `power_equal_clocks` default in the original `LoadSettings` is `false` (it reads `json.contains(...) && ...`, ignoring the struct default of `true`). Preserve that exact behavior — the test asserts it. The platform-default `sav_watch_dir` fallback uses `std::getenv`; do not assert on it in tests (environment-dependent).

- [ ] **Step 3: Add test to CMake `TEST_SOURCE_FILES`, build, verify link FAILS**

Run: `cmake --build build --config Release`
Expected: link error — `SettingsStore` symbols undefined.

- [ ] **Step 4: Implement `src/settings_store.cpp`**

Move the bodies of `LoadSettings`/`SaveSettings` from `production_app.cpp` verbatim. Apply these substitutions:
- `settings.X` → `out.X` (in `Load`) / keep `settings.X` (in `Save`, where the param is named `settings`).
- `LoadFile(settings_file.data())` → `file_store.Load(settings_path)`.
- `SaveFile(settings_file.data(), ...)` → `file_store.Save(settings_path, ...)`.
- The `for (const auto& r : Data::Recipes())` loop → iterate the passed `alt_recipes` vector instead: `for (const Recipe* r : alt_recipes) { if (r->alternate) { out.unlocked_alts[r] = ...; } }`. (This removes the hard `Data::` dependency, enabling tests.)
- The `SaveSettings()` call at the end of `Load` (when file absent) → `Save(out);`.

Keep the `#if WITH_SPOILERS_OPTIONS` block and the platform `#if defined(_WIN32)` getenv fallback verbatim. Add `#define WITH_SPOILERS_OPTIONS 0` at the top of this file (it was a `production_app.cpp`-local macro).

- [ ] **Step 5: Rewire `ProductionApp`**

In `production_app.hpp`: remove the inline `Settings` struct definition and the `LoadSettings`/`SaveSettings` declarations; add `#include "settings_store.hpp"`, keep `Settings settings;`, add `std::unique_ptr<SettingsStore> settings_store;`.

In `production_app.cpp`: delete the moved `LoadSettings`/`SaveSettings` bodies. In the constructor, after `file_store` is created, build the store and load:

```cpp
    settings_store = std::make_unique<SettingsStore>(*file_store, std::string(settings_file));
    std::vector<const Recipe*> alts;
    for (const auto& r : Data::Recipes()) alts.push_back(r.get());
    settings_store->Load(settings, alts);
```

Replace the former `LoadSettings();` call with the block above. Replace any other `SaveSettings();` call sites (find with `rg -n "SaveSettings\(\)" ficsit-companion/src/production_app.cpp`) with `settings_store->Save(settings);`.

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: all build; `[settings]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/settings_store.hpp ficsit-companion/src/settings_store.cpp ficsit-companion/tests/test_settings_store.cpp ficsit-companion/CMakeLists.txt ficsit-companion/include/production_app.hpp ficsit-companion/src/production_app.cpp
git commit -m "refactor: extract SettingsStore from ProductionApp"
```

---

## Task 4: `IEditorBackend` abstraction

Wrap the `ax::NodeEditor` free functions used outside rendering so graph mutation, serialization, and import run headless in tests.

**Files:**
- Create: `ficsit-companion/include/editor_backend.hpp`
- Create: `ficsit-companion/src/editor_backend.cpp`
- Create: `ficsit-companion/tests/test_editor_backend.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Audit which `ax::NodeEditor` calls appear in the to-be-moved bodies**

Run: `rg -n "ax::NodeEditor::(DeleteNode|DeleteLink|SetNodePosition|GetNodePosition|GetStyle|FlowDirection)" ficsit-companion/src/production_app.cpp`
Expected: confirms the call set used by `CreateLink`/`DeleteLink`/`DeleteNode`/`Deserialize`/`ImportSavFromJson`/`UpdateNodesRate`. The interface below covers them; if the audit shows additional calls, add matching methods.

- [ ] **Step 2: Create `include/editor_backend.hpp`**

```cpp
#pragma once

#include <imgui_node_editor.h>
#include <imgui.h>

/// @brief Seam over the imgui-node-editor free functions used by non-render
/// logic, so that logic can be unit-tested without a live editor context.
class IEditorBackend
{
public:
    virtual ~IEditorBackend() = default;
    virtual void DeleteNode(ax::NodeEditor::NodeId id) = 0;
    virtual void DeleteLink(ax::NodeEditor::LinkId id) = 0;
    virtual void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) = 0;
    virtual ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) = 0;
    /// @brief Style flow duration, used when flagging a propagation error.
    virtual float GetFlowDuration() = 0;
};

/// @brief Forwards to the real ax::NodeEditor functions (requires a live editor context).
class NodeEditorBackend : public IEditorBackend
{
public:
    void DeleteNode(ax::NodeEditor::NodeId id) override;
    void DeleteLink(ax::NodeEditor::LinkId id) override;
    void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) override;
    ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) override;
    float GetFlowDuration() override;
};
```

- [ ] **Step 3: Create the test fake `FakeEditorBackend` in `tests/graph_test_helpers.hpp`**

```cpp
#pragma once

#include <map>
#include <vector>
#include "editor_backend.hpp"

/// @brief Records calls and stores positions so headless tests can drive logic
/// that would otherwise need a live node-editor context.
class FakeEditorBackend : public IEditorBackend
{
public:
    void DeleteNode(ax::NodeEditor::NodeId id) override { deleted_nodes.push_back(id); }
    void DeleteLink(ax::NodeEditor::LinkId id) override { deleted_links.push_back(id); }
    void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) override
    {
        positions[id.Get()] = pos;
    }
    ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) override
    {
        auto it = positions.find(id.Get());
        return it == positions.end() ? ImVec2(0, 0) : it->second;
    }
    float GetFlowDuration() override { return flow_duration; }

    std::vector<ax::NodeEditor::NodeId> deleted_nodes;
    std::vector<ax::NodeEditor::LinkId> deleted_links;
    std::map<uintptr_t, ImVec2> positions;
    float flow_duration = 1.0f;
};
```

- [ ] **Step 4: Write the failing test `tests/test_editor_backend.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "graph_test_helpers.hpp"

TEST_CASE("FakeEditorBackend records deletions and positions", "[editor_backend]")
{
    FakeEditorBackend fake;
    fake.SetNodePosition(ax::NodeEditor::NodeId(7), ImVec2(3.0f, 4.0f));
    ImVec2 p = fake.GetNodePosition(ax::NodeEditor::NodeId(7));
    REQUIRE(p.x == 3.0f);
    REQUIRE(p.y == 4.0f);

    fake.DeleteNode(ax::NodeEditor::NodeId(7));
    REQUIRE(fake.deleted_nodes.size() == 1);
}
```

- [ ] **Step 5: Add `tests/test_editor_backend.cpp` to CMake, build — verify it FAILS to link**

Run: `cmake --build build --config Release`
Expected: link error on `NodeEditorBackend` (no `editor_backend.cpp`). The `FakeEditorBackend` is header-only so its own symbols resolve; the failure is from `editor_backend.cpp` being referenced by core. If core compiles `editor_backend.cpp` as an empty TU it will still fail because the methods are declared but not defined.

- [ ] **Step 6: Implement `src/editor_backend.cpp`**

```cpp
#include "editor_backend.hpp"

void NodeEditorBackend::DeleteNode(ax::NodeEditor::NodeId id) { ax::NodeEditor::DeleteNode(id); }
void NodeEditorBackend::DeleteLink(ax::NodeEditor::LinkId id) { ax::NodeEditor::DeleteLink(id); }
void NodeEditorBackend::SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) { ax::NodeEditor::SetNodePosition(id, pos); }
ImVec2 NodeEditorBackend::GetNodePosition(ax::NodeEditor::NodeId id) { return ax::NodeEditor::GetNodePosition(id); }
float NodeEditorBackend::GetFlowDuration() { return ax::NodeEditor::GetStyle().FlowDuration; }
```

- [ ] **Step 7: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: `[editor_backend]` test PASSES.

- [ ] **Step 8: Commit**

```bash
git add ficsit-companion/include/editor_backend.hpp ficsit-companion/src/editor_backend.cpp ficsit-companion/tests/test_editor_backend.cpp ficsit-companion/tests/graph_test_helpers.hpp ficsit-companion/CMakeLists.txt
git commit -m "refactor: add IEditorBackend seam over imgui-node-editor"
```

---

## Task 5: `RateSolver` — extract `UpdateNodesRate`

Move the ~805-line `UpdateNodesRate` (`production_app.cpp:852-1656`) into a free-standing `RateSolver` operating on the graph. This is the highest-value test target.

**Files:**
- Create: `ficsit-companion/include/rate_solver.hpp`
- Create: `ficsit-companion/src/rate_solver.cpp`
- Create: `ficsit-companion/tests/test_rate_solver.cpp`
- Modify: `production_app.hpp` / `production_app.cpp`, `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create `include/rate_solver.hpp`**

```cpp
#pragma once

#include <memory>
#include <vector>

#include "fractional_number.hpp"

struct Link;
struct Node;
struct Pin;

/// @brief Propagates item-rate updates through the node graph (exact rational
/// arithmetic). Moved verbatim from ProductionApp::UpdateNodesRate.
class RateSolver
{
public:
    /// @param nodes All nodes in the graph (errors are reset across all pins).
    /// @param links All links in the graph (flow is reset/!set during solve).
    /// @param constraint_pin The pin whose rate the user just changed.
    /// @param constraint_value The new rate for that pin.
    /// @return false if the update is rejected (caller should undo the change).
    /// @throws std::runtime_error on an internal propagation inconsistency.
    static bool Solve(std::vector<std::unique_ptr<Node>>& nodes,
                      std::vector<std::unique_ptr<Link>>& links,
                      const Pin* constraint_pin,
                      const FractionalNumber& constraint_value);
};
```

- [ ] **Step 2: Create shared graph-building helpers in `tests/graph_test_helpers.hpp`**

Append to the file created in Task 4. These let tests build small graphs with fabricated items/recipes (no `Data::LoadData`). Verify member/field names against `node.hpp`, `pin.hpp`, `recipe.hpp`, `item`/`building` headers before finalizing; adjust to match.

```cpp
#include <functional>
#include "node.hpp"
#include "pin.hpp"
#include "link.hpp"
#include "item.hpp"
#include "recipe.hpp"

/// @brief Monotonic id generator standing in for ProductionApp::GetNextId.
struct IdGen {
    unsigned long long n = 1;
    unsigned long long operator()() { return n++; }
};

/// @brief Link two pins the way ProductionApp::CreateLink wires them, minus the
/// item/rate side effects, for solver tests that only care about flow math.
inline std::unique_ptr<Link> MakeLink(unsigned long long id, Pin* out_pin, Pin* in_pin)
{
    auto link = std::make_unique<Link>(ax::NodeEditor::LinkId(id), out_pin, in_pin);
    out_pin->link = link.get();
    in_pin->link = link.get();
    return link;
}
```

> NOTE: If `Item`/`Recipe` cannot be cheaply fabricated (non-public ctors, required fields), prefer building graphs out of `MergerNode`/`CustomSplitterNode` (which take an `Item*` that may be a stack `Item{}` with just a name) rather than `CraftNode`. The solver math for mergers/splitters does not need a recipe. Confirm the minimal viable node set during implementation.

- [ ] **Step 3: Write failing tests `tests/test_rate_solver.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "graph_test_helpers.hpp"
#include "rate_solver.hpp"

// Builds: one Merger with two inputs and one output, all carrying the same item.
// Setting the output to 60 should distribute across inputs so they sum to 60.
TEST_CASE("RateSolver: merger output equals sum of inputs", "[rate_solver]")
{
    IdGen gen;
    Item item;            // fabricated; set whatever name field exists
    // item.name = "Iron";

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;

    auto merger = std::make_unique<MergerNode>(ax::NodeEditor::NodeId(gen()), std::ref(gen), &item);
    // A MergerNode is created with its default ins/outs; if it needs >1 input,
    // add pins per the MergerNode API. Capture raw pointers:
    Node* m = merger.get();
    nodes.push_back(std::move(merger));

    // Drive the output pin to 60 and solve.
    Pin* out_pin = m->outs.front().get();
    const bool ok = RateSolver::Solve(nodes, links, out_pin, FractionalNumber(60, 1));
    REQUIRE(ok);

    FractionalNumber sum(0, 1);
    for (const auto& p : m->ins) sum += p->current_rate;
    REQUIRE(sum == FractionalNumber(60, 1));
}
```

> NOTE: This test sketch encodes the *intended invariant*. During implementation, adjust node construction to the real `MergerNode`/pin API and pick whichever simple topology (balanced splitter, 2-input merger, single craft ratio) is easiest to assert exactly. Add at least: (1) a merger sum case, (2) a balanced/custom splitter split case, (3) a locked-pin constraint case. Keep assertions on exact `FractionalNumber` values.

- [ ] **Step 4: Add `tests/test_rate_solver.cpp` to CMake, build — verify FAIL to link (`RateSolver::Solve` undefined)**

Run: `cmake --build build --config Release`
Expected: link error on `RateSolver::Solve`.

- [ ] **Step 5: Implement `src/rate_solver.cpp` by moving `UpdateNodesRate` verbatim**

Cut the entire body of `ProductionApp::UpdateNodesRate` (`production_app.cpp:852-1656`) into `RateSolver::Solve`. Apply these mechanical substitutions:
- Signature: `ProductionApp::UpdateNodesRate(const Pin* constraint_pin, const FractionalNumber& constraint_value)` → `RateSolver::Solve(std::vector<std::unique_ptr<Node>>& nodes, std::vector<std::unique_ptr<Link>>& links, const Pin* constraint_pin, const FractionalNumber& constraint_value)`. (The original first param is named `constraint_pin`/`constraint_value` per line 852 — keep those names.)
- `nodes` and `links` now refer to the parameters (same names), so the loop bodies are unchanged.
- Remove the `error_time = 0.0f;` line (line ~871) — `error_time` is UI state owned by `ProductionApp`. The caller resets it (see Step 6).
- Move the file-static `DEBUG_PROPAGATION` macro and its two `graph_update_*` static sets (`production_app.cpp:46,50-53`) to the top of `rate_solver.cpp` if the body references them. Add `#define DEBUG_PROPAGATION 0`.
- Move any file-static helpers that `UpdateNodesRate` alone uses into `rate_solver.cpp` (anonymous namespace). Check with `rg` for each helper called inside the moved range; if a helper is also used by code remaining in `production_app.cpp`, instead promote it to a shared header. List of helpers to check: `IsFuelPin` (used widely — keep a copy or share via a small header).
- Add includes to `rate_solver.cpp`: `node.hpp`, `pin.hpp`, `link.hpp`, `<queue>`, `<unordered_set>`, `<stdexcept>`, `<algorithm>`, `<cstdio>`.

- [ ] **Step 6: Add a delegating wrapper in `ProductionApp` so all render call sites stay unchanged**

In `production_app.hpp`, keep the declaration `bool UpdateNodesRate(const Pin* pin, const FractionalNumber& new_rate);`. In `production_app.cpp`, replace the deleted body with:

```cpp
bool ProductionApp::UpdateNodesRate(const Pin* constraint_pin, const FractionalNumber& constraint_value)
{
    error_time = 0.0f;
    return RateSolver::Solve(nodes, links, constraint_pin, constraint_value);
}
```

Add `#include "rate_solver.hpp"` to `production_app.cpp`. (`nodes`/`links` are still `ProductionApp` members at this point — Task 6 moves them into `GraphModel` and rebinds.)

- [ ] **Step 7: Build, run tests, and verify the app still propagates**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: `[rate_solver]` tests PASS; smoke + earlier tests still pass.

- [ ] **Step 8: Commit**

```bash
git add ficsit-companion/include/rate_solver.hpp ficsit-companion/src/rate_solver.cpp ficsit-companion/tests/test_rate_solver.cpp ficsit-companion/tests/graph_test_helpers.hpp ficsit-companion/CMakeLists.txt ficsit-companion/include/production_app.hpp ficsit-companion/src/production_app.cpp
git commit -m "refactor: extract RateSolver from ProductionApp::UpdateNodesRate"
```

---

## Task 6: `GraphModel` — own `nodes`/`links` + graph mutation

Move `nodes`, `links`, `next_id`, `GetNextId`, `FindPin`, `CreateLink`, `DeleteLink`, `DeleteNode` (and the file-static organizer/extractor helpers they use) into a `GraphModel`. Use reference-member binding so rendering code referencing `nodes`/`links`/these methods is untouched.

**Files:**
- Create: `ficsit-companion/include/graph_model.hpp`
- Create: `ficsit-companion/src/graph_model.cpp`
- Create: `ficsit-companion/tests/test_graph_model.cpp`
- Modify: `production_app.hpp` / `production_app.cpp`, `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create `include/graph_model.hpp`**

```cpp
#pragma once

#include <memory>
#include <vector>

#include <imgui_node_editor.h>

#include "fractional_number.hpp"

struct Link;
struct Node;
struct Pin;
class IEditorBackend;

/// @brief Owns the node graph and all structural mutations. Pure of rendering;
/// node-editor side effects go through an injected IEditorBackend.
class GraphModel
{
public:
    explicit GraphModel(IEditorBackend& editor);

    unsigned long long int GetNextId();
    Pin* FindPin(ax::NodeEditor::PinId id) const;
    /// @brief Create a link between two pins (any order). When trigger_update is
    /// true, runs the RateSolver and undoes the link if propagation rejects it.
    /// @return false if the link was undone, or sets error_out on exception.
    void CreateLink(Pin* start, Pin* end, bool trigger_update, float* error_time_out);
    void DeleteLink(ax::NodeEditor::LinkId id);
    void DeleteNode(ax::NodeEditor::NodeId id);

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    unsigned long long int next_id = 1;

private:
    IEditorBackend& editor;
};
```

> NOTE on `CreateLink` error signaling: the original sets `error_time = ax::NodeEditor::GetStyle().FlowDuration` on a propagation exception. To keep `GraphModel` UI-free, pass `float* error_time_out`; on exception, set `*error_time_out = editor.GetFlowDuration()`. The `ProductionApp` wrapper passes `&error_time`. The `std::bind(&ProductionApp::GetNextId, this)` calls inside the moved body become `std::bind(&GraphModel::GetNextId, this)` or a lambda `[this]{ return GetNextId(); }`.

- [ ] **Step 2: Write failing tests `tests/test_graph_model.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "graph_test_helpers.hpp"
#include "graph_model.hpp"
#include "node.hpp"
#include "pin.hpp"
#include "link.hpp"

TEST_CASE("GraphModel::GetNextId increments", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    const auto a = g.GetNextId();
    const auto b = g.GetNextId();
    REQUIRE(b == a + 1);
}

TEST_CASE("GraphModel::DeleteNode removes node and its links", "[graph_model]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    // Build two organizer nodes connected by a link, then delete one and
    // assert both the node and the link are gone and the backend was told.
    // (Construct nodes via the real Node API; see graph_test_helpers.hpp.)
    // ... build nodes/links into g.nodes / g.links ...
    // ax::NodeEditor::NodeId target = g.nodes.front()->id;
    // g.DeleteNode(target);
    // REQUIRE(g.nodes.size() == 1);
    // REQUIRE(g.links.empty());
    // REQUIRE(fake.deleted_nodes.size() == 1);
}
```

> NOTE: Flesh out the second test with concrete node construction during implementation (mirror the helper usage from the rate-solver tests). Assert: deleting a node erases it from `g.nodes`, erases its incident links from `g.links`, and records the deletion on the `FakeEditorBackend`.

- [ ] **Step 3: Add test to CMake, build — verify FAIL to link**

Run: `cmake --build build --config Release`
Expected: link error on `GraphModel` symbols.

- [ ] **Step 4: Implement `src/graph_model.cpp`**

Move the bodies of `GetNextId` (430-433), `FindPin` (435-462), `CreateLink` (670-783), `DeleteLink` (785-826), `DeleteNode` (828-850) verbatim. Also move the file-static helpers used only by `CreateLink` from `production_app.cpp:464-668` into an anonymous namespace in `graph_model.cpp`: `ResolveItemThroughChain`, `ResolveOrganizerItem`, `RecalculateOrganizerItemChain`, `PropagateExtractorResourceUpstream`, and `IsFuelPin` (lines 57-67). **Important:** `RenderOrganizerRecalcButton` and `RenderOrganizerRecalcChainButton` (599-629) call ImGui and are used by rendering — leave those in `production_app.cpp`; they call `ResolveOrganizerItem`/`RecalculateOrganizerItemChain`, so those two helpers must be shared. Put `IsFuelPin`, `ResolveOrganizerItem`, `RecalculateOrganizerItemChain` (and their transitive helper `ResolveItemThroughChain`, `PropagateExtractorResourceUpstream`) into a new small shared header `include/graph_item_resolve.hpp` + `src/graph_item_resolve.cpp` in `fc-core`, and include it from both `graph_model.cpp` and `production_app.cpp`.

Substitutions inside the moved bodies:
- `ax::NodeEditor::DeleteLink(id)` → `editor.DeleteLink(id)`; `ax::NodeEditor::DeleteNode(id)` → `editor.DeleteNode(id)`.
- `UpdateNodesRate(start, start->current_rate)` (inside `CreateLink`) → `RateSolver::Solve(nodes, links, start, start->current_rate)`. Add `#include "rate_solver.hpp"`.
- The exception branch `error_time = ax::NodeEditor::GetStyle().FlowDuration;` → `if (error_time_out) *error_time_out = editor.GetFlowDuration();`.
- `std::bind(&ProductionApp::GetNextId, this)` → `[this]{ return GetNextId(); }`.
- `GetNextId()` calls → unchanged (now a `GraphModel` method).

> The new `include/graph_item_resolve.hpp` declares: `bool IsFuelPin(const Pin*)`, `const Item* ResolveItemThroughChain(Pin*)`, `const Item* ResolveOrganizerItem(Node*)`, `void RecalculateOrganizerItemChain(OrganizerNode*)`, `void PropagateExtractorResourceUpstream(Node*, const Item*, const std::function<unsigned long long int()>&)`. Move their bodies into `graph_item_resolve.cpp`. Remove the now-duplicate file-static copies from `production_app.cpp` and `rate_solver.cpp` (the rate solver should also include this header for `IsFuelPin`).

- [ ] **Step 5: Rewire `ProductionApp` with reference-member binding + delegating wrappers**

In `production_app.hpp`:
- Add `#include "graph_model.hpp"` and `#include "editor_backend.hpp"`.
- Replace the `nodes`, `links`, `next_id` member declarations with:
  ```cpp
  std::unique_ptr<IEditorBackend> editor_backend;
  GraphModel graph;
  std::vector<std::unique_ptr<Node>>& nodes = graph.nodes;
  std::vector<std::unique_ptr<Link>>& links = graph.links;
  ```
  > Reference members must be initialized in the constructor init list; see below. Keep `nodes`/`links` as references so the ~2500 lines of rendering using `nodes`/`links` compile unchanged.
- Keep the declarations of `GetNextId`, `FindPin`, `CreateLink`, `DeleteLink`, `DeleteNode` (they become wrappers).

In `production_app.cpp`:
- The constructor must initialize `editor_backend` and `graph` before the reference members. Because member init order follows declaration order and `graph` needs `*editor_backend`, declare `editor_backend` before `graph` in the header, and use an init list:
  ```cpp
  ProductionApp::ProductionApp()
      : editor_backend(std::make_unique<NodeEditorBackend>())
      , graph(*editor_backend)
  {
      // ... existing body (next_id init line removed; it lives in GraphModel) ...
  }
  ```
- Delete the moved bodies of `GetNextId`/`FindPin`/`CreateLink`/`DeleteLink`/`DeleteNode`.
- Add wrappers:
  ```cpp
  unsigned long long int ProductionApp::GetNextId() { return graph.GetNextId(); }
  Pin* ProductionApp::FindPin(ax::NodeEditor::PinId id) const { return graph.FindPin(id); }
  void ProductionApp::CreateLink(Pin* start, Pin* end, const bool trigger_update) { graph.CreateLink(start, end, trigger_update, &error_time); }
  void ProductionApp::DeleteLink(const ax::NodeEditor::LinkId id) { graph.DeleteLink(id); }
  void ProductionApp::DeleteNode(const ax::NodeEditor::NodeId id) { graph.DeleteNode(id); }
  ```
- The `UpdateNodesRate` wrapper from Task 5 already uses `nodes`/`links` (now references into `graph`) — unchanged.

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: all build; `[graph_model]` tests PASS; earlier tests still pass.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/graph_model.hpp ficsit-companion/src/graph_model.cpp ficsit-companion/include/graph_item_resolve.hpp ficsit-companion/src/graph_item_resolve.cpp ficsit-companion/tests/test_graph_model.cpp ficsit-companion/CMakeLists.txt ficsit-companion/include/production_app.hpp ficsit-companion/src/production_app.cpp ficsit-companion/src/rate_solver.cpp
git commit -m "refactor: extract GraphModel and shared item-resolve helpers"
```

---

## Task 7: `SessionSerializer` — extract `Serialize`/`Deserialize`

**Files:**
- Create: `ficsit-companion/include/session_serializer.hpp`
- Create: `ficsit-companion/src/session_serializer.cpp`
- Create: `ficsit-companion/tests/test_session_serializer.cpp`
- Modify: `production_app.hpp` / `production_app.cpp`, `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create `include/session_serializer.hpp`**

```cpp
#pragma once

#include <string>

class GraphModel;
class IEditorBackend;

/// @brief Serializes/deserializes a GraphModel to/from the .fcs JSON string.
class SessionSerializer
{
public:
    SessionSerializer(GraphModel& graph, IEditorBackend& editor, int save_version);

    std::string Serialize() const;
    void Deserialize(const std::string& s);

private:
    GraphModel& graph;
    IEditorBackend& editor;
    int save_version;
};
```

- [ ] **Step 2: Write failing test `tests/test_session_serializer.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "graph_test_helpers.hpp"
#include "graph_model.hpp"
#include "session_serializer.hpp"
#include "node.hpp"

TEST_CASE("SessionSerializer round-trips an empty graph", "[serializer]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    SessionSerializer ser(g, fake, /*save_version=*/7);

    const std::string dumped = ser.Serialize();
    REQUIRE_FALSE(dumped.empty());

    ser.Deserialize(dumped);
    REQUIRE(g.nodes.empty());
    REQUIRE(g.links.empty());
}

TEST_CASE("SessionSerializer round-trips a graph with nodes", "[serializer]")
{
    FakeEditorBackend fake;
    GraphModel g(fake);
    // Build a couple of nodes (+ one link) into g, serialize, clear by
    // deserializing into a fresh GraphModel, and assert node/link counts match.
    // (Construct nodes via the real Node API; see graph_test_helpers.hpp.)
}
```

> NOTE: Complete the second test with concrete nodes during implementation. Assert that node count and link count survive a Serialize → Deserialize round-trip. Use the `save_version` constant `7` (the current `ProductionApp::SAVE_VERSION`).

- [ ] **Step 3: Add test to CMake, build — verify FAIL to link**

Run: `cmake --build build --config Release`
Expected: link error on `SessionSerializer`.

- [ ] **Step 4: Implement `src/session_serializer.cpp`**

Move `Serialize` (299-355) and `Deserialize` (357-428) verbatim. Substitutions:
- `nodes` → `graph.nodes`; `links` → `graph.links`.
- `SAVE_VERSION` → `save_version` (the ctor param).
- `GetNextId()` → `graph.GetNextId()`; `std::bind(&ProductionApp::GetNextId, this)` → `[this]{ return graph.GetNextId(); }`.
- `ax::NodeEditor::DeleteNode(n->id)` → `editor.DeleteNode(n->id)`; `ax::NodeEditor::DeleteLink(l->id)` → `editor.DeleteLink(l->id)`; `ax::NodeEditor::SetNodePosition(...)` → `editor.SetNodePosition(...)`.
- `CreateLink(a, b, false)` → `graph.CreateLink(a, b, false, nullptr)`.
- Add includes: `graph_model.hpp`, `editor_backend.hpp`, `node.hpp`, `pin.hpp`, `link.hpp`, `json.hpp`, `game_data.hpp`. `UpdateSave`/`Data::Version()` come from `json.hpp`/`game_data.hpp`.

- [ ] **Step 5: Rewire `ProductionApp`**

In `production_app.hpp`: add `#include "session_serializer.hpp"`, add member `std::unique_ptr<SessionSerializer> session_serializer;` (declared after `graph`/`editor_backend`). Keep `Serialize`/`Deserialize` declarations (become wrappers).

In `production_app.cpp`:
- In the constructor body (after `graph` is constructed), create it:
  ```cpp
  session_serializer = std::make_unique<SessionSerializer>(graph, *editor_backend, SAVE_VERSION);
  ```
- Delete the moved `Serialize`/`Deserialize` bodies; add wrappers:
  ```cpp
  std::string ProductionApp::Serialize() const { return session_serializer->Serialize(); }
  void ProductionApp::Deserialize(const std::string& s) { session_serializer->Deserialize(s); }
  ```
  > `Serialize` is `const`; `session_serializer` is a `unique_ptr` member, so calling through it from a const method is fine (the pointer is const, the pointee is not).

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: all build; `[serializer]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/session_serializer.hpp ficsit-companion/src/session_serializer.cpp ficsit-companion/tests/test_session_serializer.cpp ficsit-companion/CMakeLists.txt ficsit-companion/include/production_app.hpp ficsit-companion/src/production_app.cpp
git commit -m "refactor: extract SessionSerializer from ProductionApp"
```

---

## Task 8: `SavImportService` — extract import orchestration + pure world-name derivation

Extract the testable world-name parsing out of `RefreshDiscoveredWorlds`, plus move the import orchestration (`ImportSavFromJson`, `ImportSavFile`, `DrainPendingImports`, `RefreshDiscoveredWorlds`, `RunSavParserDesktop`) into a `SavImportService`. `RenderSavImportSection` stays in `ProductionApp` (it is rendering).

**Files:**
- Create: `ficsit-companion/include/sav_import_service.hpp`
- Create: `ficsit-companion/src/sav_import_service.cpp`
- Create: `ficsit-companion/tests/test_sav_import_service.cpp`
- Modify: `production_app.hpp` / `production_app.cpp`, `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create `include/sav_import_service.hpp`**

Focus the testable surface on the pure world-name derivation; keep the rest as orchestration methods.

```cpp
#pragma once

#include <string>
#include <vector>

/// @brief Derive a Satisfactory world name from a .sav file stem by stripping
/// the standard suffix patterns (_CMP, _autosave_N, trailing _<digits>). Pure;
/// extracted from RefreshDiscoveredWorlds so it can be unit-tested.
std::string DeriveWorldName(std::string stem);

/// @brief Given a list of .sav file stems, return the sorted, de-duplicated set
/// of world names. Pure wrapper over DeriveWorldName.
std::vector<std::string> DiscoverWorldNames(const std::vector<std::string>& sav_stems);
```

> NOTE: The full `SavImportService` (owning `GraphModel&`, `IFileStore&`, the `sav_last_*` state, and the `ImportSav*`/`Drain*` methods) is a larger move that touches `ImGui::GetTime()` and `ax::NodeEditor` placement. Keeping those orchestration methods on `ProductionApp` for now is acceptable per the "pragmatic" scope — the high-value, genuinely testable piece is `DeriveWorldName`/`DiscoverWorldNames`. Extract only those two pure functions in this task; leave `ImportSavFromJson`/`ImportSavFile`/`DrainPendingImports` on `ProductionApp` and have `RefreshDiscoveredWorlds` call `DiscoverWorldNames`. (If full extraction is desired later, it is a follow-up.)

- [ ] **Step 2: Write failing tests `tests/test_sav_import_service.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include "sav_import_service.hpp"

TEST_CASE("DeriveWorldName strips standard suffixes", "[sav_import]")
{
    REQUIRE(DeriveWorldName("MyWorld") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_CMP") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_autosave_2") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_2024-01-02-03-04-05") == "MyWorld");
}

TEST_CASE("DiscoverWorldNames de-dupes and sorts", "[sav_import]")
{
    std::vector<std::string> stems = {
        "Beta_CMP", "Alpha_autosave_0", "Alpha", "Beta_autosave_1"
    };
    auto worlds = DiscoverWorldNames(stems);
    REQUIRE(worlds.size() == 2);
    REQUIRE(worlds[0] == "Alpha");
    REQUIRE(worlds[1] == "Beta");
}
```

- [ ] **Step 3: Add test to CMake, build — verify FAIL to link**

Run: `cmake --build build --config Release`
Expected: link error on `DeriveWorldName`/`DiscoverWorldNames`.

- [ ] **Step 4: Implement `src/sav_import_service.cpp`**

Extract the per-stem stripping logic from `RefreshDiscoveredWorlds` (`production_app.cpp:4737-4761`) into `DeriveWorldName(std::string stem)` verbatim (the `_CMP`/`_autosave_*` suffix loop + the `underscore_with_digit` trailing-pattern trim), returning the stem (empty string if it reduces to empty). `DiscoverWorldNames` runs each stem through `DeriveWorldName`, skips empties, de-dupes via `std::unordered_set`, sorts, returns. Includes: `<algorithm>`, `<cstring>`, `<cctype>`, `<unordered_set>`.

- [ ] **Step 5: Rewire `RefreshDiscoveredWorlds`**

In `production_app.cpp`, replace the inline per-file stripping inside the `directory_iterator` loop with: collect each `entry.path().stem().string()` into a `std::vector<std::string> stems`, then `discovered_worlds = DiscoverWorldNames(stems);` after the loop. Add `#include "sav_import_service.hpp"`. The `#if !defined(__EMSCRIPTEN__)` guard and `directory_iterator` stay in `ProductionApp` (filesystem traversal is environment I/O, not pure logic).

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure`
Expected: all build; `[sav_import]` tests PASS.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/sav_import_service.hpp ficsit-companion/src/sav_import_service.cpp ficsit-companion/tests/test_sav_import_service.cpp ficsit-companion/CMakeLists.txt ficsit-companion/src/production_app.cpp
git commit -m "refactor: extract pure world-name derivation for sav import"
```

---

## Task 9: Final integration verification

No new code; confirm the whole refactor preserves behavior and the file shrank meaningfully.

- [ ] **Step 1: Confirm the line count dropped**

Run: `wc -l ficsit-companion/src/production_app.cpp`
Expected: substantially below 5082 (the moved bodies — ~260 persistence/settings + ~805 rate solver + ~420 graph mutation + helpers — are gone; rendering remains). Roughly ~3000-3300 lines.

- [ ] **Step 2: Full clean build of all targets (desktop)**

Run:
```
cmake --build build --config Release --clean-first
```
Expected: `fc-core`, `ficsit-companion`, `fc-tests` all build with no warnings about the refactor.

- [ ] **Step 3: Run the complete test suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all tests pass (`[file_store] [settings] [editor_backend] [rate_solver] [graph_model] [serializer] [sav_import] [smoke]`).

- [ ] **Step 4: Manual smoke test of the running app**

Launch the built `ficsit-companion` executable and verify, by observation:
- An existing `saved/last_session.fcs` loads and renders the same graph as before the refactor.
- Creating/deleting nodes and links works; rate propagation updates as before.
- Toggling a setting (e.g. show somersloop) persists across restart (`settings.json` round-trips).
- A `.sav` import still builds a group node (if Node tooling is configured).

> Use the `verify` or `run` skill if available to drive the app. If anything differs from pre-refactor behavior, treat it as a regression and use `superpowers:systematic-debugging` before claiming completion.

- [ ] **Step 5: Final commit (only if any cleanup was needed)**

```bash
git add -A
git commit -m "refactor: finalize ProductionApp decomposition"
```

---

## Self-review notes

- **Spec coverage:** All 7 spec modules have tasks — `IFileStore` (T2), `SettingsStore` (T3), `IEditorBackend` (T4), `RateSolver` (T5), `GraphModel` (T6), `SessionSerializer` (T7), sav-import pure logic (T8). The spec's full `SavImportService` (owning state + orchestration) is intentionally narrowed in T8 to the pure `DeriveWorldName`/`DiscoverWorldNames` extraction, with full extraction flagged as optional follow-up — this is the honest pragmatic boundary (the orchestration methods are ImGui-coupled via `ImGui::GetTime()` and node placement). Catch2 `fc-tests` + `ctest` (T1). `fc-core` library (T1).
- **Type consistency:** `IFileStore::Load/Save/Remove`, `IEditorBackend::Delete*/SetNodePosition/GetNodePosition/GetFlowDuration`, `GraphModel::CreateLink(..., float* error_time_out)`, `RateSolver::Solve(nodes, links, pin, value)`, `SessionSerializer(graph, editor, save_version)`, `SettingsStore::Load(out, alt_recipes)/Save(settings)` are used consistently across tasks.
- **Behavior preservation:** every large body is *moved verbatim* with only mechanical substitutions listed; delegating wrappers + reference-member binding keep all rendering call sites untouched; `error_time` reset moves to the wrapper to keep `RateSolver` UI-free.
- **Known implementation-time confirmations (not placeholders, but require checking real APIs):** exact `MergerNode`/`Item`/`Recipe` constructor shapes for test fixtures (T5 Step 2 note); whether `utils.cpp`/`game_data.cpp` link cleanly into `fc-core` or must stay in the app target (T1 Step 7); the full `ax::NodeEditor` call set surfaced by the audit (T4 Step 1).
