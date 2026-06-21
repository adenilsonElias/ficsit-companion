# Factory Snapshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only **Factory Snapshot** tool as a third top-level tab, fed by a single shared `.sav` load that simultaneously updates all three tools (Modeler, Vehicle Map, Factory Snapshot).

**Architecture:** A new shared `SaveSource` config + an `AppHost`-level "load once, distribute to all" flow runs the `.sav` wrapper a single time and hands the resulting JSON to every tool via a new `BaseApp::LoadFromWrapperJson` virtual hook. Factory Snapshot owns a read-only `FactorySnapshotModel` (built from `SavImport::ParseWrapperJson` + `SavImport::BuildGraph` + `BuildResourceFlowReport`) and renders it as a list-first surface (topology counts, resource-flow table, warnings) with no edit affordances.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, Catch2 tests, CMake (`fc-core` static lib + `ficsit-companion` app + `fc-tests`). Exact rational math via `FractionalNumber`. JSON via `domain/json.hpp`.

---

## Scope (confirmed with user)

- The user wants **one place to load a save**, and **all three tabs load the same save**. This supersedes spec §4's "Modeler is not automatically overwritten" — the shared load feeds the Modeler too. (Modeler import is *additive*: `ImportSavFromJson` appends a `GroupNode`, it does not wipe the existing plan, so this is non-destructive.)
- Factory Snapshot's first surface is **list-first** (entity/topology summary + resource-flow table + warnings), not a node-editor canvas. A read-only node-editor canvas is deferred to a later plan.
- Existing per-tool `.sav` controls remain functional during migration; the shared load bar is the new unified entry point.

## Layering rules (must follow)

- `domain/` = pure logic, **no** infra/UI/IO deps. `infra/` may depend on `domain/`, never the reverse. `app/` may depend on both.
- Therefore: `FactorySnapshotModel` (data holder) lives in `domain/`; the builder that calls `SavImport` (infra) lives in `infra/`.
- Every new `.cpp`/`.hpp` and every new test file must be registered in `ficsit-companion/CMakeLists.txt` (there is no glob).

## File Structure

| File | Layer | Responsibility |
|------|-------|----------------|
| `include/infra/save_source.hpp` / `src/infra/save_source.cpp` | infra | Shared persisted `.sav` load config (node exe path, watch dir/world/enabled, last path) + JSON round-trip. |
| `include/domain/factory_snapshot_model.hpp` / `src/domain/factory_snapshot_model.cpp` | domain | Read-only imported state: owned `nodes`/`links`, `warnings`, `error`/`ok`, cached `ResourceFlowReport`. `Clear()`. No infra deps. |
| `include/infra/factory_snapshot_builder.hpp` / `src/infra/factory_snapshot_builder.cpp` | infra | `BuildSnapshot(ParseResult,...)` and `BuildSnapshotFromJson(json,...)` → fills a `FactorySnapshotModel` using `SavImport::BuildGraph` + `BuildResourceFlowReport`. |
| `include/infra/factory_snapshot_session.hpp` / `src/infra/factory_snapshot_session.cpp` | infra | Persisted Snapshot *view* prefs (flow filter, search, layout mode, world spacing) + JSON round-trip. |
| `include/app/factory_snapshot_app.hpp` / `src/app/factory_snapshot_app.cpp` | app | List-first read-only UI; overrides `LoadFromWrapperJson`. |
| `include/app/base_app.hpp` | app | Add `virtual void LoadFromWrapperJson(const std::string&)` no-op hook. |
| `src/app/main.cpp` | app | `AppHost` gains snapshot + `SaveSource` + `SaveWatcher`; shared load bar; 3rd tab; distribute JSON. |
| `tests/test_save_source.cpp` | test | `SaveSource` round-trip. |
| `tests/test_factory_snapshot_builder.cpp` | test | Build from `ParseResult`/JSON, pipe-junction regression, warnings, no GraphModel mutation. |
| `tests/test_factory_snapshot_session.cpp` | test | Session round-trip. |

---

## Phase 1 — Shared SaveSource config (TDD)

### Task 1: SaveSource struct + JSON round-trip

**Files:**
- Create: `ficsit-companion/include/infra/save_source.hpp`
- Create: `ficsit-companion/src/infra/save_source.cpp`
- Create: `ficsit-companion/tests/test_save_source.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `ficsit-companion/tests/test_save_source.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "infra/save_source.hpp"

/// @test Every persisted field of SaveSource survives Serialize -> Deserialize.
/// @covers SaveSource::Serialize/Deserialize for the full field set.
TEST_CASE("SaveSource round-trips every field", "[save_source]")
{
    SaveSource in;
    in.node_executable_path = "C:/node/node.exe";
    in.sav_watch_dir = "D:/saves";
    in.sav_watch_world = "Refinery";
    in.sav_watch_enabled = true;
    in.last_sav_path = "D:/saves/Refinery_autosave_1.sav";

    SaveSource out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.node_executable_path == in.node_executable_path);
    REQUIRE(out.sav_watch_dir == in.sav_watch_dir);
    REQUIRE(out.sav_watch_world == in.sav_watch_world);
    REQUIRE(out.sav_watch_enabled == in.sav_watch_enabled);
    REQUIRE(out.last_sav_path == in.last_sav_path);
}

/// @test Malformed JSON leaves a pre-set field untouched (graceful degradation).
/// @covers SaveSource::Deserialize parse-failure path.
TEST_CASE("SaveSource keeps defaults for malformed JSON", "[save_source]")
{
    SaveSource s;
    s.sav_watch_world = "Seed";
    s.Deserialize("not json {");
    REQUIRE(s.sav_watch_world == "Seed");
}

/// @test Absent keys keep current values (sparse/old-blob compatibility).
/// @covers SaveSource::Deserialize per-key merge semantics.
TEST_CASE("SaveSource keeps current values for absent keys", "[save_source]")
{
    SaveSource s;
    s.sav_watch_dir = "kept";
    s.Deserialize("{\"sav_watch_world\":\"Other\"}");
    REQUIRE(s.sav_watch_world == "Other");
    REQUIRE(s.sav_watch_dir == "kept");
}
```

- [ ] **Step 2: Run the test to verify it fails to compile (header missing)**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `infra/save_source.hpp` not found (and test not yet registered).

- [ ] **Step 3: Create the header**

Create `ficsit-companion/include/infra/save_source.hpp`:

```cpp
#pragma once

#include <string>

/// @brief Shared, persisted configuration for loading a Satisfactory `.sav`.
/// A plain data carrier with JSON (de)serialization, mirroring VehicleMapSession.
/// Owned by the AppHost so a single load can feed every tool. File I/O and the
/// actual subprocess run stay outside this struct.
struct SaveSource
{
    std::string node_executable_path;
    std::string sav_watch_dir;
    std::string sav_watch_world;
    bool sav_watch_enabled = false;
    std::string last_sav_path;

    /// @brief Serialize to a pretty-printed JSON string.
    std::string Serialize() const;
    /// @brief Parse from JSON; unknown / missing / mistyped keys keep current
    /// values. Malformed JSON leaves the struct untouched.
    void Deserialize(const std::string& json);
};
```

- [ ] **Step 4: Create the implementation**

Create `ficsit-companion/src/infra/save_source.cpp`:

```cpp
#include "infra/save_source.hpp"

#include "domain/json.hpp"

#include <exception>

std::string SaveSource::Serialize() const
{
    Json::Value v;
    v["version"] = 1;
    v["node_executable_path"] = node_executable_path;
    v["sav_watch_dir"] = sav_watch_dir;
    v["sav_watch_world"] = sav_watch_world;
    v["sav_watch_enabled"] = sav_watch_enabled;
    v["last_sav_path"] = last_sav_path;
    return v.Dump(2);
}

void SaveSource::Deserialize(const std::string& json)
{
    Json::Value v;
    try { v = Json::Parse(json); }
    catch (const std::exception&) { return; }
    if (!v.is_object()) return;

    auto str = [&](const char* k, std::string& out) { if (v.contains(k) && v[k].is_string()) out = v[k].get_string(); };
    auto boolean = [&](const char* k, bool& out) { if (v.contains(k) && v[k].is_bool()) out = v[k].get<bool>(); };

    str("node_executable_path", node_executable_path);
    str("sav_watch_dir", sav_watch_dir);
    str("sav_watch_world", sav_watch_world);
    boolean("sav_watch_enabled", sav_watch_enabled);
    str("last_sav_path", last_sav_path);
}
```

- [ ] **Step 5: Register in CMake**

In `ficsit-companion/CMakeLists.txt`, add to `HEADER_FILES` (after `include/infra/sav_runner.hpp`):

```cmake
	include/infra/save_source.hpp
```

Add to `INFRA_SOURCE_FILES` (after `src/infra/sav_runner.cpp`):

```cmake
    src/infra/save_source.cpp
```

Add to `TEST_SOURCE_FILES` (after `tests/test_settings_store.cpp`):

```cmake
        tests/test_save_source.cpp
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R save_source --output-on-failure`
Expected: PASS — 3 SaveSource test cases.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/infra/save_source.hpp ficsit-companion/src/infra/save_source.cpp ficsit-companion/tests/test_save_source.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat: add shared SaveSource config with JSON round-trip"
```

---

## Phase 2 — FactorySnapshotModel + Builder (TDD)

### Task 2: FactorySnapshotModel data holder

**Files:**
- Create: `ficsit-companion/include/domain/factory_snapshot_model.hpp`
- Create: `ficsit-companion/src/domain/factory_snapshot_model.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create the header**

Create `ficsit-companion/include/domain/factory_snapshot_model.hpp`:

```cpp
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "domain/resource_flow.hpp"

struct Node;
struct Link;

/// @brief Read-only imported factory state from a `.sav`. Owns its node/link
/// graph (built via the importer), the import warnings, and a cached per-item
/// resource-flow report. Pure data — no UI, no infra dependency. Mutation of
/// the graph is intentionally not exposed; the only state transition is Clear()
/// plus whole-graph assignment by a builder in the infra layer.
struct FactorySnapshotModel
{
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    std::vector<std::string> warnings;
    /// @brief Non-empty when the last build failed; `ok` is then false.
    std::string error;
    bool ok = false;
    /// @brief Per-item produced/consumed/net report over `nodes`.
    ResourceFlowReport flow;

    /// @brief Reset to empty (no nodes/links/warnings, ok=false, empty report).
    void Clear();

    /// @brief Count of imported nodes whose GetKind() equals `kind`.
    std::size_t CountOfKind(int kind) const;
};
```

- [ ] **Step 2: Create the implementation**

Create `ficsit-companion/src/domain/factory_snapshot_model.cpp`:

```cpp
#include "domain/factory_snapshot_model.hpp"

#include "domain/node.hpp"

void FactorySnapshotModel::Clear()
{
    nodes.clear();
    links.clear();
    warnings.clear();
    error.clear();
    ok = false;
    flow = ResourceFlowReport{};
}

std::size_t FactorySnapshotModel::CountOfKind(int kind) const
{
    std::size_t count = 0;
    for (const auto& n : nodes)
    {
        if (static_cast<int>(n->GetKind()) == kind) ++count;
    }
    return count;
}
```

- [ ] **Step 3: Register in CMake**

In `ficsit-companion/CMakeLists.txt`, add to `HEADER_FILES` (after `include/domain/fractional_number.hpp`):

```cmake
	include/domain/factory_snapshot_model.hpp
```

Add to `DOMAIN_SOURCE_FILES` (after `src/domain/fractional_number.cpp`):

```cmake
    src/domain/factory_snapshot_model.cpp
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build --config Release --target fc-core`
Expected: PASS — `fc-core` builds with the new translation unit.

- [ ] **Step 5: Commit**

```bash
git add ficsit-companion/include/domain/factory_snapshot_model.hpp ficsit-companion/src/domain/factory_snapshot_model.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat: add read-only FactorySnapshotModel data holder"
```

### Task 3: FactorySnapshotBuilder — build from ParseResult

**Files:**
- Create: `ficsit-companion/include/infra/factory_snapshot_builder.hpp`
- Create: `ficsit-companion/src/infra/factory_snapshot_builder.cpp`
- Create: `ficsit-companion/tests/test_factory_snapshot_builder.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `ficsit-companion/tests/test_factory_snapshot_builder.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "domain/factory_snapshot_model.hpp"
#include "domain/game_data.hpp"
#include "domain/node.hpp"
#include "domain/resource_flow.hpp"
#include "infra/factory_snapshot_builder.hpp"
#include "infra/sav_import.hpp"

#include "graph_test_helpers.hpp" // IdGen

namespace
{
    void EnsureGameDataLoaded()
    {
        static bool loaded = false;
        if (loaded) return;
        const std::filesystem::path source_path(__FILE__);
        const std::filesystem::path repo_root =
            source_path.parent_path().parent_path().parent_path();
        const std::filesystem::path asset_base = repo_root / "assets" / "satisfactory";
        Data::LoadData(asset_base.string());
        loaded = true;
    }

    SavImport::Building WaterExtractor(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Miner;
        b.item_name = "Water";
        b.extractor_kind = 4;
        b.clock = 1.0;
        b.x = x;
        return b;
    }

    SavImport::Building Manufacturer(const std::string& id, const std::string& recipe, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Manufacturer;
        b.recipe_name = recipe;
        b.clock = 1.0;
        b.x = x;
        return b;
    }
}

/// @test A non-empty ParseResult builds an ok snapshot whose flow report has rows.
/// @covers FactorySnapshot::BuildSnapshot success path + resource-flow wiring.
TEST_CASE("BuildSnapshot reports ok with a flow report for a craft graph", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("c1", "Iron Plate", 0.0f));

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE(model.ok);
    REQUIRE(model.error.empty());
    REQUIRE_FALSE(model.nodes.empty());
    REQUIRE_FALSE(model.flow.rows.empty());
}

/// @test Regression (spec §10): a Water Extractor producing 120/min into a Pipe
/// Junction shows 120 on the producer-side junction input in the snapshot graph.
/// @covers BuildSnapshot preserves producer-side pipe-junction rates (via BuildGraph).
TEST_CASE("BuildSnapshot keeps 120/min on producer-side pipe-junction inputs", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(WaterExtractor("water_a", 0.0f));
    parsed.buildings.push_back(WaterExtractor("water_b", 100.0f));
    parsed.buildings.push_back(Manufacturer("coal_generator", "Power (Coal)", 200.0f));

    SavImport::PipeNetwork net;
    net.id = 1;
    net.fluid = "Water";
    net.endpoints.push_back({ "water_a", "out" });
    net.endpoints.push_back({ "water_b", "out" });
    net.endpoints.push_back({ "coal_generator", "in" });
    parsed.pipe_networks.push_back(net);

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));

    const LogisticsNode* junction = nullptr;
    for (const auto& n : model.nodes)
    {
        if (n->IsLogistics()
            && static_cast<const LogisticsNode*>(n.get())->logistics_kind == LogisticsNode::Kind::PipeJunction)
        {
            junction = static_cast<const LogisticsNode*>(n.get());
            break;
        }
    }
    REQUIRE(junction != nullptr);
    REQUIRE(junction->ins.size() == 2);
    REQUIRE(junction->ins[0]->current_rate == FractionalNumber(120, 1));
    REQUIRE(junction->ins[1]->current_rate == FractionalNumber(120, 1));
}

/// @test A failed parse (ok=false) yields ok=false and a non-empty error, no nodes.
/// @covers BuildSnapshot guard against an un-parsed ParseResult.
TEST_CASE("BuildSnapshot fails cleanly on a not-ok ParseResult", "[snapshot_builder]")
{
    SavImport::ParseResult parsed; // parsed.ok defaults to false
    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE_FALSE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE_FALSE(model.ok);
    REQUIRE_FALSE(model.error.empty());
    REQUIRE(model.nodes.empty());
}
```

> Note: `graph_test_helpers.hpp` provides `IdGen` (a callable returning `unsigned long long`, usable as the id generator via `std::ref(ids)`), and `LogisticsNode` / `FractionalNumber` are reachable through `domain/node.hpp`. These mirror the existing `tests/test_sav_import.cpp` setup exactly.

- [ ] **Step 2: Run the test to verify it fails to compile (builder missing)**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `infra/factory_snapshot_builder.hpp` not found.

- [ ] **Step 3: Create the builder header**

Create `ficsit-companion/include/infra/factory_snapshot_builder.hpp`:

```cpp
#pragma once

#include <functional>
#include <string>

#include "infra/sav_import.hpp"

struct FactorySnapshotModel;

namespace FactorySnapshot
{
    /// @brief Build a read-only snapshot from an already-parsed ParseResult.
    /// Reuses SavImport::BuildGraph for topology/rates, then computes the
    /// resource-flow report over the built nodes. On failure returns false and
    /// fills model.error (model is left cleared). On success model.ok = true and
    /// model owns the nodes/links/warnings/flow. Existing model state is cleared
    /// first either way.
    bool BuildSnapshot(const SavImport::ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err);

    /// @brief Convenience: parse wrapper JSON then BuildSnapshot. A parse error
    /// is surfaced through model.error / the bool result.
    bool BuildSnapshotFromJson(const std::string& wrapper_json,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err);
}
```

- [ ] **Step 4: Create the builder implementation**

Create `ficsit-companion/src/infra/factory_snapshot_builder.cpp`:

```cpp
#include "infra/factory_snapshot_builder.hpp"

#include "domain/factory_snapshot_model.hpp"
#include "domain/resource_flow.hpp"

namespace FactorySnapshot
{
    bool BuildSnapshot(const SavImport::ParseResult& parsed,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err)
    {
        model.Clear();

        if (!parsed.ok)
        {
            model.error = parsed.error.empty() ? "Save data did not parse" : parsed.error;
            err = model.error;
            return false;
        }

        SavImport::BuildOutput built;
        std::string build_err;
        if (!SavImport::BuildGraph(parsed, id_generator, built, build_err, options))
        {
            model.error = build_err.empty() ? "Failed to build snapshot graph" : build_err;
            err = model.error;
            return false;
        }

        model.nodes = std::move(built.nodes);
        model.links = std::move(built.links);
        model.warnings = std::move(built.warnings);
        model.flow = BuildResourceFlowReport(model.nodes);
        model.ok = true;
        return true;
    }

    bool BuildSnapshotFromJson(const std::string& wrapper_json,
        const std::function<unsigned long long int()>& id_generator,
        const SavImport::BuildOptions& options,
        FactorySnapshotModel& model,
        std::string& err)
    {
        const SavImport::ParseResult parsed = SavImport::ParseWrapperJson(wrapper_json);
        return BuildSnapshot(parsed, id_generator, options, model, err);
    }
}
```

- [ ] **Step 5: Register in CMake**

In `ficsit-companion/CMakeLists.txt`, add to `HEADER_FILES` (after the new `include/infra/save_source.hpp`):

```cmake
	include/infra/factory_snapshot_builder.hpp
```

Add to `INFRA_SOURCE_FILES` (after the new `src/infra/save_source.cpp`):

```cmake
    src/infra/factory_snapshot_builder.cpp
```

Add to `TEST_SOURCE_FILES` (after `tests/test_sav_import_service.cpp`):

```cmake
        tests/test_factory_snapshot_builder.cpp
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R snapshot_builder --output-on-failure`
Expected: PASS — 3 snapshot_builder cases, including the 120/min regression.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/infra/factory_snapshot_builder.hpp ficsit-companion/src/infra/factory_snapshot_builder.cpp ficsit-companion/tests/test_factory_snapshot_builder.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat: add FactorySnapshot builder (ParseResult/JSON -> read-only model)"
```

### Task 4: Builder retains warnings and does not touch a modeler GraphModel

**Files:**
- Modify: `ficsit-companion/tests/test_factory_snapshot_builder.cpp`

- [ ] **Step 1: Add the warnings + isolation tests**

Append to `ficsit-companion/tests/test_factory_snapshot_builder.cpp`:

```cpp
#include "domain/graph_model.hpp"

/// @test Importer warnings are carried onto the snapshot model verbatim.
/// @covers BuildSnapshot copies BuildOutput::warnings into the model.
TEST_CASE("BuildSnapshot retains importer warnings", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    // A merger fed two different item streams emits a warning in BuildGraph.
    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("plate", "Iron Plate", 0.0f));
    parsed.buildings.push_back(Manufacturer("rod", "Iron Rod", 50.0f));
    SavImport::Building merger;
    merger.id = "m";
    merger.kind = SavImport::BuildingKind::Merger;
    merger.x = 100.0f;
    parsed.buildings.push_back(merger);
    parsed.belts.push_back({ "b1", "", "plate", 0, "", "out", "m", 0, "", "in" });
    parsed.belts.push_back({ "b2", "", "rod", 0, "", "out", "m", 1, "", "in" });

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));
    REQUIRE_FALSE(model.warnings.empty());
}

/// @test Building a snapshot leaves a separately-constructed modeler GraphModel
/// completely empty — snapshot ownership never leaks into the planner.
/// @covers spec §10 "Snapshot load does not mutate a modeler GraphModel".
TEST_CASE("BuildSnapshot does not mutate a separate GraphModel", "[snapshot_builder]")
{
    EnsureGameDataLoaded();

    GraphModel planner; // independent modeler graph
    const std::size_t before = planner.GetNodes().size();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Manufacturer("c1", "Iron Plate", 0.0f));

    IdGen ids;
    FactorySnapshotModel model;
    std::string err;
    REQUIRE(FactorySnapshot::BuildSnapshot(parsed, std::ref(ids), {}, model, err));

    REQUIRE(planner.GetNodes().size() == before);
}
```

> Before running, confirm the `GraphModel` accessor name. Open `ficsit-companion/include/domain/graph_model.hpp` and verify the node-list getter is `GetNodes()`. If it differs (e.g. `Nodes()`), use the actual name in the assertion. The merger-warning belt fixture mirrors `tests/test_sav_import.cpp`'s "merger receives different item streams" case — if `SavImport::Belt`'s aggregate field order differs, copy that test's exact `parsed.belts.push_back({...})` literal instead.

- [ ] **Step 2: Run the tests to verify they pass**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R snapshot_builder --output-on-failure`
Expected: PASS — now 5 snapshot_builder cases.

- [ ] **Step 3: Commit**

```bash
git add ficsit-companion/tests/test_factory_snapshot_builder.cpp
git commit -m "test: snapshot builder retains warnings and isolates the modeler graph"
```

---

## Phase 3 — FactorySnapshotSession (TDD)

### Task 5: Snapshot view-preferences session

**Files:**
- Create: `ficsit-companion/include/infra/factory_snapshot_session.hpp`
- Create: `ficsit-companion/src/infra/factory_snapshot_session.cpp`
- Create: `ficsit-companion/tests/test_factory_snapshot_session.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `ficsit-companion/tests/test_factory_snapshot_session.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "infra/factory_snapshot_session.hpp"

/// @test Every persisted view-pref field survives Serialize -> Deserialize.
/// @covers FactorySnapshotSession::Serialize/Deserialize full field set.
TEST_CASE("FactorySnapshotSession round-trips every field", "[snapshot_session]")
{
    FactorySnapshotSession in;
    in.flow_filter = 2;          // Surplus
    in.flow_search = "water";
    in.world_layout = true;

    FactorySnapshotSession out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.flow_filter == in.flow_filter);
    REQUIRE(out.flow_search == in.flow_search);
    REQUIRE(out.world_layout == in.world_layout);
}

/// @test Malformed JSON leaves a pre-set field untouched.
TEST_CASE("FactorySnapshotSession keeps defaults for malformed JSON", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.flow_search = "kept";
    s.Deserialize("nope {");
    REQUIRE(s.flow_search == "kept");
}

/// @test An out-of-range flow_filter is clamped to 0 (All).
TEST_CASE("FactorySnapshotSession clamps invalid flow_filter", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"flow_filter\": 99}");
    REQUIRE(s.flow_filter == 0);
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — `infra/factory_snapshot_session.hpp` not found.

- [ ] **Step 3: Create the header**

Create `ficsit-companion/include/infra/factory_snapshot_session.hpp`:

```cpp
#pragma once

#include <string>

/// @brief Persisted *view* preferences for the Factory Snapshot tool (the shared
/// .sav load config lives in SaveSource, not here). A plain data carrier with
/// JSON (de)serialization, mirroring VehicleMapSession. flow_filter encodes
/// ResourceFlowFilter (0=All, 1=Deficit, 2=Surplus).
struct FactorySnapshotSession
{
    int flow_filter = 0;
    std::string flow_search;
    bool world_layout = false;

    std::string Serialize() const;
    /// @brief Per-key merge; malformed JSON is ignored; flow_filter out of
    /// [0,2] is clamped to 0.
    void Deserialize(const std::string& json);
};
```

- [ ] **Step 4: Create the implementation**

Create `ficsit-companion/src/infra/factory_snapshot_session.cpp`:

```cpp
#include "infra/factory_snapshot_session.hpp"

#include "domain/json.hpp"

#include <exception>

std::string FactorySnapshotSession::Serialize() const
{
    Json::Value v;
    v["version"] = 1;
    v["flow_filter"] = flow_filter;
    v["flow_search"] = flow_search;
    v["world_layout"] = world_layout;
    return v.Dump(2);
}

void FactorySnapshotSession::Deserialize(const std::string& json)
{
    Json::Value v;
    try { v = Json::Parse(json); }
    catch (const std::exception&) { return; }
    if (!v.is_object()) return;

    if (v.contains("flow_filter") && v["flow_filter"].is_number()) flow_filter = v["flow_filter"].get<int>();
    if (v.contains("flow_search") && v["flow_search"].is_string()) flow_search = v["flow_search"].get_string();
    if (v.contains("world_layout") && v["world_layout"].is_bool()) world_layout = v["world_layout"].get<bool>();

    if (flow_filter < 0 || flow_filter > 2) flow_filter = 0;
}
```

- [ ] **Step 5: Register in CMake**

Add to `HEADER_FILES` (after `include/infra/factory_snapshot_builder.hpp`):

```cmake
	include/infra/factory_snapshot_session.hpp
```

Add to `INFRA_SOURCE_FILES` (after `src/infra/factory_snapshot_builder.cpp`):

```cmake
    src/infra/factory_snapshot_session.cpp
```

Add to `TEST_SOURCE_FILES` (after `tests/test_factory_snapshot_builder.cpp`):

```cmake
        tests/test_factory_snapshot_session.cpp
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R snapshot_session --output-on-failure`
Expected: PASS — 3 snapshot_session cases.

- [ ] **Step 7: Commit**

```bash
git add ficsit-companion/include/infra/factory_snapshot_session.hpp ficsit-companion/src/infra/factory_snapshot_session.cpp ficsit-companion/tests/test_factory_snapshot_session.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat: add FactorySnapshotSession view-preference persistence"
```

---

## Phase 4 — App layer: shared load hook, Factory Snapshot tab, distribution

> These tasks touch ImGui/SDL UI and the subprocess loader, which are not unit-testable in `fc-tests`. Each ends with a **build** of the full `ficsit-companion` target and a brief **manual** verification. Do not claim completion without the build passing (see `superpowers:verification-before-completion`).

### Task 6: BaseApp gains a shared LoadFromWrapperJson hook

**Files:**
- Modify: `ficsit-companion/include/app/base_app.hpp`

- [ ] **Step 1: Add the virtual hook**

In `ficsit-companion/include/app/base_app.hpp`, add a default no-op virtual inside `class BaseApp`, right after `virtual void SaveSession() = 0;`:

```cpp
    /// @brief Feed this tool the JSON produced by one shared `.sav` wrapper run.
    /// Default is a no-op so a tool can opt out. Overrides must be read-only with
    /// respect to other tools' state (they own only their own model).
    virtual void LoadFromWrapperJson(const std::string& wrapper_json) { (void)wrapper_json; }
```

Add `#include <string>` to the top of the file (after `#include <chrono>`).

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add ficsit-companion/include/app/base_app.hpp
git commit -m "feat: add BaseApp::LoadFromWrapperJson shared-load hook"
```

### Task 7: VehicleMapApp consumes the shared JSON

**Files:**
- Modify: `ficsit-companion/include/app/vehicle_map_app.hpp`
- Modify: `ficsit-companion/src/app/vehicle_map_app.cpp`

- [ ] **Step 1: Declare the override**

In `ficsit-companion/include/app/vehicle_map_app.hpp`, in the `public:` section after `virtual void SaveSession() override;`:

```cpp
    /// @brief Parse the shared wrapper JSON's logistics block into the map model.
    virtual void LoadFromWrapperJson(const std::string& wrapper_json) override;
```

- [ ] **Step 2: Refactor LoadSavFile to split run vs consume, and implement the override**

In `ficsit-companion/src/app/vehicle_map_app.cpp`, replace the body of `VehicleMapApp::LoadSavFile` so the JSON-consuming half lives in the new override. The existing `LoadSavFile` runs the wrapper then delegates:

```cpp
void VehicleMapApp::LoadSavFile(const std::string& sav_path)
{
    last_error.clear();
#if defined(__EMSCRIPTEN__)
    (void)sav_path;
    last_error = "Loading .sav is not supported on the web build yet";
#else
    std::string err;
    const std::string json = SavRunner::RunSavWrapper(sav_path, node_executable_path, err);
    if (!err.empty() || json.empty())
    {
        last_error = err.empty() ? "Parser produced no output" : err;
        return;
    }
    LoadFromWrapperJson(json);
    if (last_error.empty()) last_sav_path = sav_path;
#endif
}

void VehicleMapApp::LoadFromWrapperJson(const std::string& wrapper_json)
{
    last_error.clear();
    VehicleMap::Model parsed = VehicleMap::ParseLogisticsJson(wrapper_json);
    if (!parsed.ok)
    {
        last_error = parsed.error.empty() ? "Failed to parse logistics data" : parsed.error;
        return;
    }

    last_warnings = VehicleMap::ExtractLogisticsWarnings(wrapper_json);

    model = std::move(parsed);
    needs_fit_on_load = !model.has_bounds ? false : true;

    if (!sel_station.empty() && model.station_index.find(sel_station) == model.station_index.end()) sel_station.clear();
    if (!sel_vehicle.empty() && model.vehicle_index.find(sel_vehicle) == model.vehicle_index.end()) sel_vehicle.clear();

    std::ostringstream st;
    st << model.stations.size() << " stations, " << model.vehicles.size()
       << " vehicles, " << model.segments.size() << " path segments";
    status_text = st.str();
}
```

> This preserves the original behavior exactly — the `last_sav_path` assignment moves into `LoadSavFile` because the shared-host path supplies its own path bookkeeping. `<sstream>` is already included (it was used by the original body); confirm the `#include <sstream>` is present near the top of the file and add it if the build complains.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add ficsit-companion/include/app/vehicle_map_app.hpp ficsit-companion/src/app/vehicle_map_app.cpp
git commit -m "refactor: VehicleMapApp consumes shared wrapper JSON via LoadFromWrapperJson"
```

### Task 8: ProductionApp consumes the shared JSON

**Files:**
- Modify: `ficsit-companion/include/app/production_app.hpp`
- Modify: `ficsit-companion/src/app/production_app.cpp`

- [ ] **Step 1: Declare the override**

In `ficsit-companion/include/app/production_app.hpp`, add to the `public:` section near the other `BaseApp` overrides:

```cpp
    /// @brief Import the shared wrapper JSON as a new GroupNode (additive — does
    /// not replace the current plan). Delegates to the existing import path.
    virtual void LoadFromWrapperJson(const std::string& wrapper_json) override;
```

> Verify `ImportSavFromJson` is declared in this header (it is implemented in `production_app.cpp:3636`). If `ImportSavFromJson` is currently `private`, that is fine — the override is a public method that calls it internally; no visibility change needed.

- [ ] **Step 2: Implement the override**

In `ficsit-companion/src/app/production_app.cpp`, add directly above `void ProductionApp::ImportSavFromJson(const std::string& wrapper_json)` (around line 3636):

```cpp
void ProductionApp::LoadFromWrapperJson(const std::string& wrapper_json)
{
    ImportSavFromJson(wrapper_json);
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add ficsit-companion/include/app/production_app.hpp ficsit-companion/src/app/production_app.cpp
git commit -m "feat: ProductionApp consumes shared wrapper JSON (additive import)"
```

### Task 9: FactorySnapshotApp — list-first read-only tool

**Files:**
- Create: `ficsit-companion/include/app/factory_snapshot_app.hpp`
- Create: `ficsit-companion/src/app/factory_snapshot_app.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create the header**

Create `ficsit-companion/include/app/factory_snapshot_app.hpp`:

```cpp
#pragma once

#include <string>

#include "app/base_app.hpp"
#include "domain/factory_snapshot_model.hpp"
#include "domain/resource_flow.hpp"
#include "infra/factory_snapshot_session.hpp"

/// @brief Third top-level tool: a read-only view of the imported `.sav` factory.
/// It owns a FactorySnapshotModel built from the shared wrapper JSON and renders
/// it list-first: a topology summary, a resource-flow table, and a warnings
/// panel. It exposes no graph-mutation affordances (no add/delete/rate edits).
class FactorySnapshotApp : public BaseApp
{
public:
    FactorySnapshotApp();
    virtual ~FactorySnapshotApp() override;

    virtual void SaveSession() override;
    /// @brief Build the read-only snapshot from the shared wrapper JSON.
    virtual void LoadFromWrapperJson(const std::string& wrapper_json) override;

protected:
    virtual void RenderImpl() override;

private:
    void LoadSession();

    void RenderStatusLine();
    void RenderTopologySummary();
    void RenderResourceFlowTable();
    void RenderWarningsPanel();

    unsigned long long int NextId();

    FactorySnapshotModel model;
    FactorySnapshotSession session;

    unsigned long long int next_id = 1;
    std::string last_error;
    std::string status_text;
};
```

- [ ] **Step 2: Create the implementation**

Create `ficsit-companion/src/app/factory_snapshot_app.cpp`:

```cpp
#include "app/factory_snapshot_app.hpp"

#include <imgui.h>

#include <sstream>

#include "domain/item.hpp"            // Item::name (adjust include if Item lives elsewhere)
#include "domain/node.hpp"
#include "infra/factory_snapshot_builder.hpp"
#include "infra/file_store.hpp"

namespace
{
    constexpr const char* kSessionFile = "saved/factory_snapshot.json";

    const char* KindLabel(Node::Kind kind)
    {
        switch (kind)
        {
            case Node::Kind::Craft:          return "Machines";
            case Node::Kind::CustomSplitter: return "Custom Splitters";
            case Node::Kind::Merger:         return "Mergers";
            case Node::Kind::Group:          return "Groups";
            case Node::Kind::GameSplitter:   return "Splitters";
            case Node::Kind::Sink:           return "Sinks";
            case Node::Kind::Extractor:      return "Extractors";
            case Node::Kind::Logistics:      return "Logistics";
        }
        return "Other";
    }
}

FactorySnapshotApp::FactorySnapshotApp()
{
    LoadSession();
}

FactorySnapshotApp::~FactorySnapshotApp() = default;

unsigned long long int FactorySnapshotApp::NextId()
{
    return next_id++;
}

void FactorySnapshotApp::LoadSession()
{
    std::string content;
    if (FileStore::ReadFile(kSessionFile, content))
    {
        session.Deserialize(content);
    }
}

void FactorySnapshotApp::SaveSession()
{
    FileStore::WriteFile(kSessionFile, session.Serialize());
}

void FactorySnapshotApp::LoadFromWrapperJson(const std::string& wrapper_json)
{
    last_error.clear();
    SavImport::BuildOptions options;
    options.layout_mode = session.world_layout ? SavImport::LayoutMode::World : SavImport::LayoutMode::Compact;

    std::string err;
    if (!FactorySnapshot::BuildSnapshotFromJson(wrapper_json,
            [this] { return NextId(); }, options, model, err))
    {
        last_error = err;
        status_text.clear();
        return;
    }

    std::ostringstream st;
    st << model.nodes.size() << " buildings, " << model.flow.rows.size() << " item flows, "
       << model.warnings.size() << " warning(s)";
    status_text = st.str();
}

void FactorySnapshotApp::RenderImpl()
{
    RenderStatusLine();
    ImGui::Separator();

    ImGui::BeginChild("##snapshot_left", ImVec2(280.0f, 0.0f), true);
    RenderTopologySummary();
    ImGui::Separator();
    RenderWarningsPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##snapshot_right", ImVec2(0.0f, 0.0f), true);
    RenderResourceFlowTable();
    ImGui::EndChild();
}

void FactorySnapshotApp::RenderStatusLine()
{
    ImGui::TextUnformatted("Factory Snapshot (read-only)");
    ImGui::SameLine();
    if (!last_error.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "  %s", last_error.c_str());
    }
    else if (!status_text.empty())
    {
        ImGui::TextDisabled("  %s", status_text.c_str());
    }
    else
    {
        ImGui::TextDisabled("  Load a save from the bar above to populate this view.");
    }
}

void FactorySnapshotApp::RenderTopologySummary()
{
    ImGui::TextUnformatted("Topology");
    if (model.nodes.empty())
    {
        ImGui::TextDisabled("(nothing imported)");
        return;
    }
    for (int k = 0; k <= static_cast<int>(Node::Kind::Logistics); ++k)
    {
        const std::size_t count = model.CountOfKind(k);
        if (count == 0) continue;
        ImGui::BulletText("%s: %zu", KindLabel(static_cast<Node::Kind>(k)), count);
    }
}

void FactorySnapshotApp::RenderWarningsPanel()
{
    ImGui::Text("Warnings (%zu)", model.warnings.size());
    if (model.warnings.empty())
    {
        ImGui::TextDisabled("(none)");
        return;
    }
    for (const std::string& w : model.warnings)
    {
        ImGui::TextWrapped("- %s", w.c_str());
    }
}

void FactorySnapshotApp::RenderResourceFlowTable()
{
    ImGui::TextUnformatted("Resource flow");

    int filter = session.flow_filter;
    ImGui::RadioButton("All", &filter, 0); ImGui::SameLine();
    ImGui::RadioButton("Deficit", &filter, 1); ImGui::SameLine();
    ImGui::RadioButton("Surplus", &filter, 2);
    if (filter != session.flow_filter) { session.flow_filter = filter; SaveSession(); }

    if (ImGui::InputText("Search##flow", &session.flow_search))
    {
        SaveSession();
    }

    const ResourceFlowFilter flow_filter = static_cast<ResourceFlowFilter>(session.flow_filter);

    if (ImGui::BeginTable("##flow_table", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Produced");
        ImGui::TableSetupColumn("Consumed");
        ImGui::TableSetupColumn("Net");
        ImGui::TableHeadersRow();

        for (ResourceFlowRow& row : model.flow.rows)
        {
            if (!RowPassesFilter(row, flow_filter, session.flow_search)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.item ? row.item->name.c_str() : "(unknown)");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.produced.GetStringFraction().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.consumed.GetStringFraction().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.net.GetStringFraction().c_str());
        }
        ImGui::EndTable();
    }
}
```

> Implementation notes for the engineer:
> - `ImGui::InputText(..., &std::string)` requires `imgui_stdlib`. The codebase already uses this form (`production_app.cpp` and `vehicle_map_app.cpp` call `ImGui::InputText("...", &settings.field)`), so the include is already wired through `imgui.cmake`. If the build reports the overload missing, add `#include <misc/cpp/imgui_stdlib.h>`.
> - `GetStringFraction()` is non-const, hence the loop binds `ResourceFlowRow&` (the report is owned by `model`, so mutating the cached string form is fine). `Item::name` is the display name (first ctor arg in `recipe.hpp`/`building.hpp` `Item`). **Verify the exact include** for `Item`: grep `struct Item` to confirm whether it is declared in `domain/recipe.hpp` (likely) rather than a separate `domain/item.hpp`; fix the `#include` accordingly.
> - `FileStore::ReadFile/WriteFile` — confirm the exact API in `include/infra/file_store.hpp` (the planner/vehicle map persist through it). If the signatures differ (e.g. return `std::string` directly or take different args), match the existing call sites in `vehicle_map_app.cpp`/`production_app.cpp`.

- [ ] **Step 3: Register in CMake**

In `ficsit-companion/CMakeLists.txt`:

Add to `HEADER_FILES` (after `include/app/base_app.hpp`):

```cmake
	include/app/factory_snapshot_app.hpp
```

Add `src/app/factory_snapshot_app.cpp` to **both** `APP_SOURCE_FILES` (after `src/app/vehicle_map_app.cpp`) and `APP_PROJECT_SOURCES` (after `src/app/vehicle_map_app.cpp`):

```cmake
    src/app/factory_snapshot_app.cpp
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS. Resolve any `Item`/`FileStore`/`imgui_stdlib` include mismatches per the notes above.

- [ ] **Step 5: Commit**

```bash
git add ficsit-companion/include/app/factory_snapshot_app.hpp ficsit-companion/src/app/factory_snapshot_app.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat: add list-first read-only FactorySnapshotApp"
```

### Task 10: Shared load bar + 3rd tab + distribute in AppHost

**Files:**
- Modify: `ficsit-companion/src/app/main.cpp`

- [ ] **Step 1: Extend AppHost with the snapshot tool, shared SaveSource, and a SaveWatcher**

In `ficsit-companion/src/app/main.cpp`, add includes after the existing app includes (around line 29):

```cpp
#include "app/factory_snapshot_app.hpp"
#include "infra/save_source.hpp"
#include "infra/save_watcher.hpp"
#include "infra/sav_runner.hpp"
#include <imgui_stdlib.h>
```

Replace the `AppHost` struct (lines 34-42) with:

```cpp
struct AppHost
{
    ProductionApp production;
    VehicleMapApp vehicle;
    FactorySnapshotApp snapshot;
    int active = 0;

    SaveSource source;
    SaveWatcher watcher;

    std::string load_error;
    std::string load_status;

    BaseApp* Active()
    {
        switch (active)
        {
            case 1:  return static_cast<BaseApp*>(&vehicle);
            case 2:  return static_cast<BaseApp*>(&snapshot);
            default: return static_cast<BaseApp*>(&production);
        }
    }
    void SaveAll() { production.SaveSession(); vehicle.SaveSession(); snapshot.SaveSession(); }

    // Run the wrapper once and hand the same JSON to every tool.
    void LoadSav(const std::string& sav_path)
    {
        load_error.clear();
#if defined(__EMSCRIPTEN__)
        (void)sav_path;
        load_error = "Loading .sav is not supported on the web build yet";
#else
        std::string err;
        const std::string json = SavRunner::RunSavWrapper(sav_path, source.node_executable_path, err);
        if (!err.empty() || json.empty())
        {
            load_error = err.empty() ? "Parser produced no output" : err;
            return;
        }
        production.LoadFromWrapperJson(json);
        vehicle.LoadFromWrapperJson(json);
        snapshot.LoadFromWrapperJson(json);
        source.last_sav_path = sav_path;
        load_status = "Loaded " + sav_path;
#endif
    }

    void PollWatch()
    {
        std::vector<std::string> pending;
        watcher.TakePending(pending);
        if (!pending.empty()) LoadSav(pending.back());
    }
};
```

> Confirm `SaveWatcher`'s drain method name. Grep `include/infra/save_watcher.hpp` — the planner calls `save_watcher.TakePending(pending)` and `save_watcher.Reconfigure(dir, world)`. Use whatever the header actually declares.

- [ ] **Step 2: Render a shared load bar above the tab bar and add the 3rd tab**

In `Render(...)`, replace the tab-bar block (lines 89-94) with a shared load bar followed by the three-tab switcher:

```cpp
    // Shared .sav load bar: one load feeds every tool.
    {
        ImGui::TextUnformatted("Save:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("##sav_path", &host->source.last_sav_path);
        ImGui::SameLine();
        if (ImGui::Button("Load") && !host->source.last_sav_path.empty())
        {
            host->LoadSav(host->source.last_sav_path);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("Watch folder", &host->source.sav_watch_dir))
        {
            host->watcher.Reconfigure(host->source.sav_watch_dir, host->source.sav_watch_world);
        }
        if (!host->load_error.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", host->load_error.c_str());
        }
        else if (!host->load_status.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", host->load_status.c_str());
        }
    }
    host->PollWatch();

    if (ImGui::BeginTabBar("##tools", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Production Planner")) { host->active = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Vehicle Map")) { host->active = 1; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Factory Snapshot")) { host->active = 2; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
```

- [ ] **Step 3: Build the full app**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS.

- [ ] **Step 4: Manual verification**

Run the built `ficsit-companion` executable. Confirm:
1. A "Factory Snapshot" tab appears next to "Production Planner" and "Vehicle Map".
2. The shared **Save** bar renders above the tabs.
3. Loading a `.sav` (path in the bar + **Load**) populates: the Snapshot topology summary + resource-flow table + warnings; the Vehicle Map; and adds a GroupNode to the Production Planner — from a single click.
4. The Factory Snapshot tab shows **no** add-node menu, delete controls, lock toggles, or editable rate fields.

Document the observed result (counts seen, any warnings) in the commit message or PROGRESS notes.

- [ ] **Step 5: Commit**

```bash
git add ficsit-companion/src/app/main.cpp
git commit -m "feat: shared .sav load bar feeds all three tools + Factory Snapshot tab"
```

---

## Phase 5 — Full verification

### Task 11: Whole-suite green + final build

- [ ] **Step 1: Build and run the entire test suite**

Run:
```bash
cmake --build build --config Release --target fc-tests
ctest --test-dir build -C Release --output-on-failure
```
Expected: PASS — all tests, including the new `save_source`, `snapshot_builder`, and `snapshot_session` cases, and **no regressions** in `sav_import`, `resource_flow`, or `vehicle_map_*`.

- [ ] **Step 2: Build the desktop app one final time**

Run: `cmake --build build --config Release --target ficsit-companion`
Expected: PASS.

- [ ] **Step 3: Update the design spec status**

In `docs/superpowers/specs/2026-06-16-factory-snapshot-design.md`, change the header `**Status:** Draft, pending user review` to `**Status:** Implemented (Phase 1 — shared load + list-first snapshot)`, and add a one-line note that the shared-load behavior (all three tabs load one save) supersedes §4's "not automatically overwritten" per user decision.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-06-16-factory-snapshot-design.md
git commit -m "docs: mark factory-snapshot design as implemented (phase 1)"
```

---

## Spec coverage check

| Spec requirement | Covered by |
|------------------|-----------|
| §2 Factory Snapshot as a separate top-level tool | Task 9 (app) + Task 10 (tab) |
| §3 Show machines/logistics, warnings, resource-flow totals | Task 9 render methods |
| §3 No node/link/rate/recipe editing; not saved as modeler session | Task 9 (read-only UI; separate session/model) |
| §6 `FactorySnapshotModel` (domain), `FactorySnapshotSession` (infra), snapshot builder reusing `BuildGraph`/`BuildResourceFlowReport` | Tasks 2, 3, 5 |
| §7 Load flow: wrapper → JSON → ParseWrapperJson → snapshot build → UI | Tasks 3, 9, 10 |
| §8 Load/watch controls, status line, list-first view, flow table w/ filters, warnings panel | Tasks 9, 10 |
| §10 Session round-trip; wrapper JSON → snapshot; producer-side junction rate; flow report; no modeler mutation | Tasks 5, 3, 3 (regression), 3, 4 |
| §10 Regression: Water Extractor 120/min → Pipe Junction shows 120 | Task 3 Step 1 (second test) |
| User override: one shared load feeds all three tabs | Tasks 6, 7, 8, 10 |

**Deferred (out of this plan, by user choice / spec migration staging):** read-only node-editor canvas (§12), spec migration Steps 2–3 (relocate/remove Modeler import), and the Snapshot↔Modeler bridge (§5).
