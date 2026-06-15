# `.sav` Import — Vehicle Routes via Plugs + Fuel Pin Color — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `.sav` importer build truck/train stations as `VehicleStationNode`s, wire vehicle routes as plug↔plug route links, auto-balance each route pool, and give the station fuel inlet a distinct color in the modeler.

**Architecture:** The modeler already models stations as `VehicleStationNode` (plug + `route_links` + `VehicleRoute` pool balancing). This plan brings the importer onto that same mechanism: it constructs `VehicleStationNode`s (mode from the save's `is_unloader`), replaces the old cargo-pin route wiring with plug route links, and reuses the modeler's pool-solve (promoted into the `VehicleRoute` domain module) to balance imported routes.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, Catch2 tests, CMake/MSVC.

**Spec:** `docs/superpowers/specs/2026-06-14-sav-import-vehicle-routes-design.md`

> **Note — no git (user preference):** Do NOT create commits or branches. After each task, run the full test suite and tick the boxes here; record running notes in a PROGRESS file if useful. The "Commit" step in the standard template is replaced by "Run full suite + tick boxes" below.

---

## File Structure

| File | Change | Responsibility |
|------|--------|----------------|
| `ficsit-companion/include/domain/node.hpp` | Modify | Declare new `VehicleStationNode` constructor overload. |
| `ficsit-companion/src/domain/vehicle_station_node.cpp` | Modify | Define the new constructor. |
| `ficsit-companion/include/domain/vehicle_route.hpp` | Modify | Declare promoted `ResolveRoutePool` / `SyncRoutePool`. |
| `ficsit-companion/src/domain/vehicle_route.cpp` | Modify | Define promoted helpers (move from graph_model). |
| `ficsit-companion/src/domain/graph_model.cpp` | Modify | Remove local helpers; call `VehicleRoute::` versions. |
| `ficsit-companion/src/infra/sav_import.cpp` | Modify | Build `VehicleStationNode`s; plug route links; pool solve. |
| `ficsit-companion/src/app/production_app.cpp` | Modify | Color the fuel inlet pin. |
| `ficsit-companion/tests/test_vehicle_station_node.cpp` | Modify | Test the new constructor. |
| `ficsit-companion/tests/test_sav_import.cpp` | Modify | Test station kind/mode, plug route links, pool balancing. |

---

## Task 1: `VehicleStationNode` constructor with explicit mode + cargo counts

**Files:**
- Modify: `ficsit-companion/include/domain/node.hpp` (after the existing `VehicleStationNode` constructors, ~line 242)
- Modify: `ficsit-companion/src/domain/vehicle_station_node.cpp`
- Test: `ficsit-companion/tests/test_vehicle_station_node.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_vehicle_station_node.cpp` (after the existing construction tests, before the closing of the file):

```cpp
/// @test The importer constructor builds the requested cargo pin counts plus a
///       trailing fuel inlet, and the plug direction follows the given mode.
/// @covers VehicleStationNode(id, kind, mode, cargo_in, cargo_out, id_generator).
TEST_CASE("VehicleStationNode importer ctor honors mode and cargo counts", "[vehicle_station]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };

    VehicleStationNode loader(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, VehicleStationNode::Mode::Load,
        /*cargo_in=*/1, /*cargo_out=*/2, gen);
    REQUIRE(loader.ins.size() == 2);   // 1 cargo + 1 fuel inlet (last)
    REQUIRE(loader.outs.size() == 2);
    REQUIRE(loader.mode == VehicleStationNode::Mode::Load);
    REQUIRE(loader.plug != nullptr);
    REQUIRE(loader.plug->direction == ax::NodeEditor::PinKind::Output);

    VehicleStationNode unloader(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TrainStation, VehicleStationNode::Mode::Unload,
        /*cargo_in=*/0, /*cargo_out=*/1, gen);
    REQUIRE(unloader.ins.size() == 1);  // 0 cargo + 1 fuel inlet
    REQUIRE(unloader.outs.size() == 1);
    REQUIRE(unloader.mode == VehicleStationNode::Mode::Unload);
    REQUIRE(unloader.plug->direction == ax::NodeEditor::PinKind::Input);
}
```

- [ ] **Step 2: Build the test target and verify it fails to compile**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL — no matching `VehicleStationNode` constructor with 6 args.

- [ ] **Step 3: Declare the constructor in `node.hpp`**

In `include/domain/node.hpp`, inside `struct VehicleStationNode`, add after the existing `(id, logistics_kind, id_generator)` constructor (line ~240):

```cpp
    /// @brief Build with an explicit mode and cargo pin counts (used by the
    /// .sav importer, which derives counts from observed belt ports). Allocates
    /// `cargo_in` cargo inputs + 1 fuel inlet (last input) + `cargo_out` cargo
    /// outputs, and a plug whose direction matches `mode`.
    VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
        Mode mode, size_t cargo_in, size_t cargo_out,
        const std::function<unsigned long long int()>& id_generator);
```

- [ ] **Step 4: Define the constructor in `vehicle_station_node.cpp`**

In `src/domain/vehicle_station_node.cpp`, add after the existing `(id, logistics_kind, id_generator)` constructor (after line 28):

```cpp
VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
    Mode mode, size_t cargo_in, size_t cargo_out,
    const std::function<unsigned long long int()>& id_generator)
    // cargo_in cargo inputs + 1 fuel inlet (last input), cargo_out cargo outputs.
    : LogisticsNode(id, logistics_kind, cargo_in + 1, cargo_out, id_generator),
      mode(mode)
{
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}
```

- [ ] **Step 5: Build and run the new test**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "importer ctor" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass (was 103 cases / 462 assertions baseline — now +1 case).

---

## Task 2: Promote `ResolveRoutePool` / `SyncRoutePool` into `VehicleRoute`

**Files:**
- Modify: `ficsit-companion/include/domain/vehicle_route.hpp`
- Modify: `ficsit-companion/src/domain/vehicle_route.cpp`
- Modify: `ficsit-companion/src/domain/graph_model.cpp:16-57` (remove local defs), call sites at `:135`, `:245`, `:313-314`

This is a pure refactor guarded by the existing `[vehicle_route]` / `[graph_model]` tests — no new test.

- [ ] **Step 1: Declare the helpers in `vehicle_route.hpp`**

In `include/domain/vehicle_route.hpp`, add `#include <memory>` to the includes, add forward declarations `struct Node;` and `struct Link;` near the existing forward declarations (after line 9), and add inside the `VehicleRoute` namespace (after `SummarizePool`, before the closing brace):

```cpp
    /// @brief Re-solve a route pool, seeding from a pool member's active cargo
    /// pin that already carries a non-zero rate. Returns false only if a solve
    /// ran and was rejected; returns true when the pool carries no rate yet.
    bool ResolveRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                          std::vector<std::unique_ptr<Link>>& links,
                          const std::vector<VehicleStationNode*>& pool,
                          float& error_time, float error_flow_duration);

    /// @brief Carry cargo item types across a route pool (PropagateCargoItems),
    /// then re-balance it (ResolveRoutePool). Returns false only if the balance
    /// solve was rejected.
    bool SyncRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const std::vector<VehicleStationNode*>& pool,
                       float& error_time, float error_flow_duration);
```

- [ ] **Step 2: Define the helpers in `vehicle_route.cpp`**

In `src/domain/vehicle_route.cpp`, ensure these includes are present (add any missing): `#include "domain/node.hpp"`, `#include "domain/pin.hpp"`, `#include "domain/link.hpp"`, `#include "domain/rate_solver.hpp"`, `#include "domain/graph_item_resolve.hpp"`. Then add inside `namespace VehicleRoute`:

```cpp
bool ResolveRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                      std::vector<std::unique_ptr<Link>>& links,
                      const std::vector<VehicleStationNode*>& pool,
                      float& error_time, float error_flow_duration)
{
    for (VehicleStationNode* s : pool)
    {
        for (const auto& p : s->ins)
        {
            if (IsActiveCargoPin(p.get()) && p->item != nullptr && p->current_rate.GetNumerator() != 0)
            {
                return RateSolver::Solve(nodes, links, p.get(), p->current_rate, error_time, error_flow_duration);
            }
        }
        for (const auto& p : s->outs)
        {
            if (IsActiveCargoPin(p.get()) && p->item != nullptr && p->current_rate.GetNumerator() != 0)
            {
                return RateSolver::Solve(nodes, links, p.get(), p->current_rate, error_time, error_flow_duration);
            }
        }
    }
    return true;
}

bool SyncRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                   std::vector<std::unique_ptr<Link>>& links,
                   const std::vector<VehicleStationNode*>& pool,
                   float& error_time, float error_flow_duration)
{
    PropagateCargoItems(pool);
    return ResolveRoutePool(nodes, links, pool, error_time, error_flow_duration);
}
```

(These are the exact bodies currently in `graph_model.cpp:22-56`, minus the `VehicleRoute::` qualifier on `PropagateCargoItems`/`SummarizePool` which are now in the same namespace.)

- [ ] **Step 3: Remove the local helpers from `graph_model.cpp`**

Delete the anonymous-namespace `ResolveRoutePool` and `SyncRoutePool` definitions at `graph_model.cpp:16-57` (the whole `namespace { ... }` block holding only those two functions and their comments). Leave the `#include "domain/vehicle_route.hpp"` (line 10) in place.

- [ ] **Step 4: Qualify the call sites**

In `graph_model.cpp`, change the three call sites to use the namespace:
- `:135` `SyncRoutePool(...)` → `VehicleRoute::SyncRoutePool(...)`
- `:245` `SyncRoutePool(...)` → `VehicleRoute::SyncRoutePool(...)`
- `:313` and `:314` `ResolveRoutePool(...)` → `VehicleRoute::ResolveRoutePool(...)`

- [ ] **Step 5: Build and run the route/graph suites**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "vehicle_route|graph_model" --output-on-failure`
Expected: PASS (behavior unchanged).

- [ ] **Step 6: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass.

---

## Task 3: Importer builds stations as `VehicleStationNode` with mode

**Files:**
- Modify: `ficsit-companion/src/infra/sav_import.cpp:942-948` (station node construction)
- Test: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_sav_import.cpp` (after the last test, line ~483). It needs `#include "domain/node.hpp"` (already present) — `VehicleStationNode` is declared there.

```cpp
/// @test Truck/train stations import as VehicleStationNode, with mode derived
///       from is_unloader and a vehicle plug whose direction matches the mode.
/// @covers SavImport::BuildGraph station node type + mode.
TEST_CASE("BuildGraph imports stations as VehicleStationNode with mode", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err));

    REQUIRE(out.nodes.size() == 2);
    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[0].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);
    REQUIRE(loader->mode == VehicleStationNode::Mode::Load);
    REQUIRE(unloader->mode == VehicleStationNode::Mode::Unload);
    REQUIRE(loader->plug->direction == ax::NodeEditor::PinKind::Output);
    REQUIRE(unloader->plug->direction == ax::NodeEditor::PinKind::Input);
}
```

If `imgui_node_editor.h` is not already included in this test file, add `#include <imgui_node_editor.h>` near the top includes (it is needed for `ax::NodeEditor::PinKind`).

- [ ] **Step 2: Build and verify failure**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "imports stations as VehicleStationNode" --output-on-failure`
Expected: FAIL — `dynamic_cast` returns null (current code builds plain `LogisticsNode`).

- [ ] **Step 3: Branch the station construction in `sav_import.cpp`**

Replace the block at `sav_import.cpp:942-948`:

```cpp
                const int cargo_in = static_cast<int>(observed_in_ports[b.id].size());
                const int cargo_out = static_cast<int>(observed_out_ports[b.id].size());
                const size_t total_in = static_cast<size_t>(std::max(default_cargo_inputs, cargo_in))
                    + (has_fuel_input_pin ? 1u : 0u);
                const size_t total_out = static_cast<size_t>(std::max(default_cargo_outputs, cargo_out));
                node = std::make_unique<LogisticsNode>(id_generator(), kind,
                    total_in, total_out, id_generator);
                break;
```

with:

```cpp
                const int cargo_in = static_cast<int>(observed_in_ports[b.id].size());
                const int cargo_out = static_cast<int>(observed_out_ports[b.id].size());
                const size_t resolved_cargo_in = static_cast<size_t>(std::max(default_cargo_inputs, cargo_in));
                const size_t resolved_cargo_out = static_cast<size_t>(std::max(default_cargo_outputs, cargo_out));
                if (kind == LogisticsNode::Kind::TruckStation || kind == LogisticsNode::Kind::TrainStation)
                {
                    // Stations are VehicleStationNode: a Load/Unload mode + a
                    // vehicle plug. Mode comes from the save (mIsInLoadMode).
                    // The constructor appends the fuel inlet as the last input.
                    const auto mode = b.is_unloader
                        ? VehicleStationNode::Mode::Unload
                        : VehicleStationNode::Mode::Load;
                    node = std::make_unique<VehicleStationNode>(id_generator(), kind, mode,
                        resolved_cargo_in, resolved_cargo_out, id_generator);
                }
                else
                {
                    const size_t total_in = resolved_cargo_in + (has_fuel_input_pin ? 1u : 0u);
                    node = std::make_unique<LogisticsNode>(id_generator(), kind,
                        total_in, resolved_cargo_out, id_generator);
                }
                break;
```

(`has_fuel_input_pin` is only ever true for stations, which now take the `VehicleStationNode` branch where the constructor adds the fuel inlet — so the `else` branch's fuel term is always 0; it is kept for clarity.)

- [ ] **Step 4: Build and run the new test**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "imports stations as VehicleStationNode" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass. In particular the existing station fuel/cargo belt tests (`routes a station fuel belt`, `splits a loader station's fuel and cargo belts`, `routes an unloader's input belt to fuel`) must still pass — the pin layout (fuel inlet last) is unchanged.

---

## Task 4: Wire vehicle routes as plug↔plug route links

**Files:**
- Modify: `ficsit-companion/src/infra/sav_import.cpp:1411-1562` (replace the `connect_vehicle_routes` block's link-creation logic)
- Test: `ficsit-companion/tests/test_sav_import.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_sav_import.cpp`:

```cpp
/// @test With connect_vehicle_routes, a route of one loader + one unloader is
///       wired by a plug<->plug route link recorded on both stations'
///       route_links, with no cargo-pin route Link created.
/// @covers SavImport::BuildGraph plug-based vehicle route wiring.
TEST_CASE("BuildGraph wires vehicle routes as plug route links", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("iron_miner", "Iron Ore", -100.0f));
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));
    // Factory belt feeds the loader's cargo input so it has a cargo pin + rate.
    parsed.belts.push_back(Belt("iron_to_loader", "iron_miner", 0, "loader", 0));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader", "guid-loader" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "unloader", "guid-unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[2].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);
    // One route link, indexed on both stations, connecting the two plugs.
    REQUIRE(loader->route_links.size() == 1);
    REQUIRE(unloader->route_links.size() == 1);
    Link* rl = loader->route_links.front();
    REQUIRE(rl == unloader->route_links.front());
    REQUIRE(rl->start == loader->plug.get());
    REQUIRE(rl->end == unloader->plug.get());
    // Plugs carry no Pin::link (route links live in route_links only).
    REQUIRE(loader->plug->link == nullptr);
    REQUIRE(unloader->plug->link == nullptr);
}
```

- [ ] **Step 2: Build and verify failure**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "wires vehicle routes as plug route links" --output-on-failure`
Expected: FAIL — `route_links` empty (old code wires cargo pins, not plugs).

- [ ] **Step 3: Replace the route-wiring block**

In `sav_import.cpp`, add `#include "domain/vehicle_route.hpp"` and `#include "domain/graph_item_resolve.hpp"` to the includes (after line 9). Then replace the entire `if (options.connect_vehicle_routes && !parsed.vehicle_routes.empty()) { ... }` block (`:1420-1562`) with:

```cpp
        if (options.connect_vehicle_routes && !parsed.vehicle_routes.empty())
        {
            // Map each logistics-block station id to its VehicleStationNode.
            std::unordered_map<std::string, VehicleStationNode*> stations;
            for (const LogisticsStation& ls : parsed.logistics_stations)
            {
                auto idx = id_to_index.find(ls.id);
                if (idx == id_to_index.end()) continue;
                Node* node = out.nodes[idx->second].get();
                if (auto* v = dynamic_cast<VehicleStationNode*>(node)) stations[ls.id] = v;
            }

            // Plug links go Load-plug (Output) -> Unload-plug (Input). Connecting
            // every loader to every unloader in a route puts the whole route in
            // one VehicleRoute pool. Pin::link stays null on plugs; the link is
            // recorded in each station's route_links (matches the modeler).
            std::set<std::pair<const void*, const void*>> created;
            size_t route_links_made = 0;
            for (const std::vector<std::string>& route : parsed.vehicle_routes)
            {
                std::vector<VehicleStationNode*> loaders, unloaders;
                for (const std::string& sid : route)
                {
                    auto it = stations.find(sid);
                    if (it == stations.end()) continue;
                    if (it->second->mode == VehicleStationNode::Mode::Load) loaders.push_back(it->second);
                    else unloaders.push_back(it->second);
                }
                for (VehicleStationNode* loader : loaders)
                {
                    for (VehicleStationNode* unloader : unloaders)
                    {
                        if (loader == unloader) continue;
                        const auto key = std::make_pair(
                            static_cast<const void*>(loader), static_cast<const void*>(unloader));
                        if (!created.insert(key).second) continue;
                        out.links.emplace_back(std::make_unique<Link>(
                            id_generator(), loader->plug.get(), unloader->plug.get()));
                        Link* rl = out.links.back().get();
                        loader->route_links.push_back(rl);
                        unloader->route_links.push_back(rl);
                        route_links_made += 1;
                    }
                }
            }

            out.warnings.push_back("[vehicle routes] " + std::to_string(route_links_made)
                + " station link(s) created");
        }
```

- [ ] **Step 4: Build and run the new test**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "wires vehicle routes as plug route links" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass.

---

## Task 5: Auto-balance each route pool after import

**Files:**
- Modify: `ficsit-companion/src/infra/sav_import.cpp` (add a step AFTER the best-effort rate-propagation fixed-point pass, which ends near `:1640`)
- Test: `ficsit-companion/tests/test_sav_import.cpp`

The route links must already exist (Task 4), and the loader's cargo-input rate must already be filled by the existing propagation pass — so the pool solve runs *after* that pass.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_sav_import.cpp`:

```cpp
/// @test After wiring a plug route, the importer balances the pool so the
///       loader's incoming cargo rate appears on the unloader's cargo output.
/// @covers SavImport::BuildGraph route pool auto-balance on import.
TEST_CASE("BuildGraph balances cargo rate across an imported route pool", "[sav_import]")
{
    EnsureGameDataLoaded();

    SavImport::ParseResult parsed;
    parsed.ok = true;
    parsed.buildings.push_back(Miner("iron_miner", "Iron Ore", -100.0f)); // Mk1 normal = 60/min
    parsed.buildings.push_back(TruckStation("loader", 0.0f, "Coal", "Iron Ore", false));
    parsed.buildings.push_back(TruckStation("unloader", 100.0f, "Coal", "Iron Ore", true));
    // Belt the miner into the loader cargo input, and the unloader output onward
    // to a sink so the unloader has a live (connected) cargo output to balance.
    parsed.buildings.push_back(Sink("sink", 200.0f));
    parsed.belts.push_back(Belt("iron_to_loader", "iron_miner", 0, "loader", 0));
    parsed.belts.push_back(Belt("unloader_to_sink", "unloader", 0, "sink", 0));
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "loader", "guid-loader" });
    parsed.logistics_stations.push_back(SavImport::LogisticsStation{ "unloader", "guid-unloader" });
    parsed.vehicle_routes.push_back(std::vector<std::string>{ "loader", "unloader" });

    SavImport::BuildOptions opts;
    opts.connect_vehicle_routes = true;

    IdGen ids;
    SavImport::BuildOutput out;
    std::string err;
    REQUIRE(SavImport::BuildGraph(parsed, std::ref(ids), out, err, opts));

    auto* loader = dynamic_cast<VehicleStationNode*>(out.nodes[1].get());
    auto* unloader = dynamic_cast<VehicleStationNode*>(out.nodes[2].get());
    REQUIRE(loader != nullptr);
    REQUIRE(unloader != nullptr);

    // Loader cargo input carries the miner's rate (non-zero).
    const Pin* loader_in = loader->ins.front().get();
    REQUIRE(loader_in->item != nullptr);
    REQUIRE(loader_in->current_rate.GetNumerator() != 0);

    // The unloader's connected cargo output now carries the same rate + item.
    bool balanced = false;
    for (const auto& p : unloader->outs)
    {
        if (p->link != nullptr && p->item != nullptr
            && p->current_rate == loader_in->current_rate)
        {
            balanced = true;
        }
    }
    REQUIRE(balanced);
}
```

If a `Sink` factory helper does not exist in this test file, add one alongside the other builders (top anonymous namespace):

```cpp
    SavImport::Building Sink(const std::string& id, const float x)
    {
        SavImport::Building b;
        b.id = id;
        b.kind = SavImport::BuildingKind::Sink;
        b.x = x;
        return b;
    }
```

- [ ] **Step 2: Build and verify failure**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "balances cargo rate across an imported route pool" --output-on-failure`
Expected: FAIL — the unloader output rate stays 0 (no pool solve yet).

- [ ] **Step 3: Add the pool-solve step after the propagation pass**

In `sav_import.cpp`, locate the end of the best-effort rate-propagation fixed-point loop (the `for (int iter = 0; iter < kMaxRatePropagationIters; ++iter)` block that starts near `:1578`). Immediately AFTER that loop closes, add:

```cpp
        // ---- Step: balance imported vehicle route pools ----
        // Now that loaders carry their cargo-input rate (filled above), settle
        // each route pool the way the interactive editor does on connect:
        // carry cargo items across the pool, then run the route solver seeded
        // from a member's live cargo pin. Best-effort: a rejected/over-
        // constrained solve is caught and leaves links + rates intact.
        if (options.connect_vehicle_routes)
        {
            std::unordered_set<VehicleStationNode*> visited;
            float import_error_time = 0.0f;
            size_t pools_balanced = 0, pools_rejected = 0;
            for (auto& node_ptr : out.nodes)
            {
                auto* v = dynamic_cast<VehicleStationNode*>(node_ptr.get());
                if (v == nullptr || v->route_links.empty()) continue;
                if (!visited.insert(v).second) continue;
                std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(v);
                for (VehicleStationNode* member : pool) visited.insert(member);
                try
                {
                    if (VehicleRoute::SyncRoutePool(out.nodes, out.links, pool, import_error_time, 0.0f))
                        pools_balanced += 1;
                    else
                        pools_rejected += 1;
                }
                catch (const std::runtime_error&)
                {
                    pools_rejected += 1;
                }
            }
            if (pools_rejected > 0)
            {
                out.warnings.push_back("[vehicle routes] " + std::to_string(pools_rejected)
                    + " route pool(s) could not be balanced (left unbalanced)");
            }
        }
```

- [ ] **Step 4: Build and run the new test**

Run: `cmake --build build --config Release --target fc-tests`
Then: `ctest --test-dir build -C Release -R "balances cargo rate across an imported route pool" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass.

---

## Task 6: Distinct color for the station fuel inlet pin

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp:1944-1951` (input-pin circle draw)

This is a renderer-only change (no headless unit test); verified by build + visual check.

- [ ] **Step 1: Color the fuel pin in the input-pin draw**

In `production_app.cpp`, replace the circle-draw block at `:1944-1951`:

```cpp
                                if (p->link == nullptr)
                                {
                                    draw_list->AddCircle(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
                                else
                                {
                                    draw_list->AddCircleFilled(center, radius, ImColor(1.0f, 1.0f, 1.0f));
                                }
```

with:

```cpp
                                // The dedicated fuel inlet on a Truck/Train station
                                // reads in the same orange as the vehicle plug so it
                                // is visually distinct from cargo belts.
                                const ImColor pin_outline = IsFuelPin(p.get())
                                    ? ImColor(255, 170, 0)
                                    : ImColor(1.0f, 1.0f, 1.0f);
                                if (p->link == nullptr)
                                {
                                    draw_list->AddCircle(center, radius, pin_outline);
                                }
                                else
                                {
                                    draw_list->AddCircleFilled(center, radius, pin_outline);
                                }
```

(`IsFuelPin` is already used elsewhere in this file — e.g. `:2840` — so its declaration from `domain/graph_item_resolve.hpp` is in scope.)

- [ ] **Step 2: Build the desktop app**

Run: `cmake --build build --config Release`
Expected: builds clean.

- [ ] **Step 3: Visual verification**

Launch the app, place a Truck Station (or import a `.sav` with stations), and confirm the fuel inlet pin (last input) renders orange while cargo pins stay white. Confirm the vehicle plug is unaffected.

- [ ] **Step 4: Run full suite + tick boxes**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all pass (no test affected by the cosmetic change).

---

## Self-Review notes

- **Spec coverage:** §1 station node → Task 1+3; §2 fuel color → Task 6; §3 plug route wiring → Task 4; §4 auto-balance + helper promotion → Task 2+5; §5 testing → tests embedded in Tasks 1,3,4,5.
- **Type consistency:** new constructor signature `(id, kind, mode, cargo_in, cargo_out, id_generator)` used identically in Task 1 (decl/def) and Task 3 (call). `VehicleRoute::SyncRoutePool` / `ResolveRoutePool` signatures match across Task 2 (decl/def) and Task 5 (call). `route_links` is `std::vector<Link*>` (per `node.hpp:258`) — `.push_back`, `.size()`, `.front()` used consistently.
- **No placeholders:** every code/command step shows concrete content.
