# Vehicle (Truck/Train) Station Node — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the manually-placed truck station node a Load/Unload mode and a distinct, multi-linkable vehicle plug that connects stations into shared routes, plus a route-summary panel — without touching the rate solver (Phase 2).

**Architecture:** A new `VehicleStationNode : public LogisticsNode` keyed off `logistics_kind` (Truck/Train); `GetKind()` stays `Node::Kind::Logistics`. The belt layout (2 cargo-in + 1 fuel-in, 2 cargo-out) already exists and is reused unchanged. The vehicle plug is a `Pin` stored **outside** `ins`/`outs` as `node->plug`; its single `Pin::link` stays unused. Plug↔plug "route links" live in the normal `GraphModel::links` vector (so the node editor draws them and they auto-clean on node delete) but are also indexed per-node in `route_links`. Because plug pins are never in any node's `ins`/`outs` and their `Pin::link` is null, the existing `RateSolver` / item-resolve BFS never reaches them — the plug subsystem is invisible to propagation in Phase 1.

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, `FractionalNumber` exact rationals, Catch2 tests, CMake.

**Source of truth:** `docs/superpowers/specs/2026-06-13-vehicle-station-node-design.md`.

**Progress tracking:** This repo uses **no git commits during work** (user preference). After each task, instead of committing, tick the task's checkboxes here and append a one-line entry to `docs/superpowers/plans/2026-06-13-vehicle-station-node-PROGRESS.md` (create it on Task 1). Run the build/tests as written; do not run `git commit`/`git add`.

---

## File Structure

| File | Responsibility | Action |
|------|----------------|--------|
| `ficsit-companion/include/domain/node.hpp` | `VehicleStationNode` decl, `Mode`, `plug`, `route_links` | Modify |
| `ficsit-companion/src/domain/vehicle_station_node.cpp` | `VehicleStationNode` impl (ctor/serialize/mode/plug) | Create |
| `ficsit-companion/include/domain/vehicle_route.hpp` | Pool discovery + summary API | Create |
| `ficsit-companion/src/domain/vehicle_route.cpp` | Pool discovery + summary impl | Create |
| `ficsit-companion/src/domain/node_base.cpp` | Route Truck/Train logistics_kind to subclass in `Deserialize` | Modify |
| `ficsit-companion/src/domain/graph_model.cpp` | Plug-link create/delete + node-delete cleanup | Modify |
| `ficsit-companion/src/app/production_app.cpp` | Menu, FindPin, plug render, mode toggle, link rules, summary panel | Modify |
| `ficsit-companion/CMakeLists.txt` | Add new `.cpp` files to lib + test sources | Modify |
| `ficsit-companion/tests/test_vehicle_station_node.cpp` | Model, serialization, pool, summary tests | Create |

---

## Task 1: `VehicleStationNode` skeleton + pin/plug layout

**Files:**
- Modify: `ficsit-companion/include/domain/node.hpp` (after `LogisticsNode`, near line 230)
- Create: `ficsit-companion/src/domain/vehicle_station_node.cpp`
- Modify: `ficsit-companion/CMakeLists.txt` (add the new source to the library target)
- Create: `ficsit-companion/tests/test_vehicle_station_node.cpp`
- Modify: `ficsit-companion/CMakeLists.txt` (add the test source to the `fc-tests` target)
- Create: `docs/superpowers/plans/2026-06-13-vehicle-station-node-PROGRESS.md`

- [ ] **Step 1: Write the failing test** (`tests/test_vehicle_station_node.cpp`)

```cpp
#include <catch2/catch_test_macros.hpp>

#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "graph_test_helpers.hpp" // IdGen

#include <imgui_node_editor.h>

/// @test A truck VehicleStationNode builds with 3 inputs (2 cargo + 1 fuel),
///       2 cargo outputs, a vehicle plug, and defaults to Load mode whose plug
///       is an Output (the route source).
/// @covers VehicleStationNode construction, default mode, plug direction.
TEST_CASE("VehicleStationNode builds belts + plug in Load mode", "[vehicle_station]")
{
    IdGen idgen;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });

    REQUIRE(s.IsLogistics());
    REQUIRE(s.logistics_kind == LogisticsNode::Kind::TruckStation);
    REQUIRE(s.ins.size() == 3);   // 2 cargo + 1 fuel
    REQUIRE(s.outs.size() == 2);  // 2 cargo
    REQUIRE(s.plug != nullptr);
    REQUIRE(s.mode == VehicleStationNode::Mode::Load);
    REQUIRE(s.plug->direction == ax::NodeEditor::PinKind::Output);
}

/// @test Switching to Unload flips the plug to an Input and clears any route links.
/// @covers VehicleStationNode::SetMode plug recreation.
TEST_CASE("VehicleStationNode SetMode flips plug direction", "[vehicle_station]")
{
    IdGen idgen;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });

    s.SetMode(VehicleStationNode::Mode::Unload, [&idgen] { return idgen(); });
    REQUIRE(s.mode == VehicleStationNode::Mode::Unload);
    REQUIRE(s.plug->direction == ax::NodeEditor::PinKind::Input);
    REQUIRE(s.route_links.empty());
}
```

- [ ] **Step 2: Add the declaration** to `node.hpp` immediately after the `LogisticsNode` struct (before `ExtractorNode`):

```cpp
/// @brief Truck/Train station with a Load/Unload mode and a vehicle "plug"
/// (a Pin held outside ins/outs) that links station-to-station to form routes.
/// Belt layout is inherited from LogisticsNode (2 cargo-in + 1 fuel-in, 2 cargo-out).
struct VehicleStationNode : public LogisticsNode
{
    enum class Mode : int { Load = 0, Unload = 1 };

    VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
        const std::function<unsigned long long int()>& id_generator);
    VehicleStationNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
        const Json::Value& serialized, const INodeDataResolver& resolver);
    virtual ~VehicleStationNode();

    virtual Json::Value Serialize() const override;

    /// @brief Recreate the plug with the direction implied by the new mode and
    /// drop existing route links (the caller must delete the Link objects first).
    void SetMode(Mode m, const std::function<unsigned long long int()>& id_generator);
    static ax::NodeEditor::PinKind PlugDirectionFor(Mode m);

    Mode mode;
    /// @brief Vehicle plug. Lives outside ins/outs; its Pin::link stays unused
    /// (route links are tracked in route_links instead).
    std::unique_ptr<Pin> plug;
    /// @brief Non-owning index of route (plug<->plug) links touching this plug.
    /// The Link objects are owned by GraphModel::links.
    std::vector<Link*> route_links;
};
```

- [ ] **Step 3: Implement** `src/domain/vehicle_station_node.cpp`:

```cpp
#include "domain/json.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/pin.hpp"

#include <imgui_node_editor.h>

namespace
{
    // Truck/Train stations always carry 2 cargo + 1 fuel inputs and 2 cargo outputs.
    constexpr size_t kCargoIns = 2;
    constexpr size_t kCargoOuts = 2;
}

ax::NodeEditor::PinKind VehicleStationNode::PlugDirectionFor(Mode m)
{
    return m == Mode::Load ? ax::NodeEditor::PinKind::Output : ax::NodeEditor::PinKind::Input;
}

VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
    const std::function<unsigned long long int()>& id_generator)
    // kCargoIns + 1 fuel inlet (last input), kCargoOuts cargo outputs.
    : LogisticsNode(id, logistics_kind, kCargoIns + 1, kCargoOuts, id_generator),
      mode(Mode::Load)
{
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}

VehicleStationNode::VehicleStationNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
    const Json::Value& serialized, const INodeDataResolver& resolver)
    : LogisticsNode(id, id_generator, serialized, resolver)
{
    mode = static_cast<Mode>(serialized.contains("mode") ? serialized["mode"].get<int>() : 0);
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
    // route_links are reconstructed by the session loader after all nodes exist.
}

VehicleStationNode::~VehicleStationNode()
{
}

void VehicleStationNode::SetMode(Mode m, const std::function<unsigned long long int()>& id_generator)
{
    if (mode == m && plug != nullptr)
    {
        return;
    }
    mode = m;
    route_links.clear();
    plug = std::make_unique<Pin>(id_generator(), PlugDirectionFor(mode), this, nullptr);
}

Json::Value VehicleStationNode::Serialize() const
{
    Json::Value serialized = LogisticsNode::Serialize();
    serialized["mode"] = static_cast<int>(mode);
    return serialized;
}
```

> Note: route-link serialization is handled by the session serializer in Task 6/7, not here, because links reference pins across nodes.

- [ ] **Step 4: Register sources in CMake.** In `ficsit-companion/CMakeLists.txt`, add `src/domain/vehicle_station_node.cpp` and `src/domain/vehicle_route.cpp` (created in Task 4) to the same source list that already contains `src/domain/logistics_node.cpp`. Add `tests/test_vehicle_station_node.cpp` to the same list that contains `tests/test_node_serialization.cpp`.

Run: `grep -n "logistics_node.cpp" ficsit-companion/CMakeLists.txt` to find the list, then add the new entries beside it.

- [ ] **Step 5: Build the test target**

Run: `cmake --build build --config Release --target fc-tests`
Expected: compiles; new tests are picked up.

- [ ] **Step 6: Run the new tests**

Run: `ctest --test-dir build -C Release -R vehicle_station --output-on-failure`
Expected: both `[vehicle_station]` cases PASS.

- [ ] **Step 7: Record progress.** Create `docs/superpowers/plans/2026-06-13-vehicle-station-node-PROGRESS.md` with a header and a first line: `- Task 1 done: VehicleStationNode skeleton + plug layout, tests green.` Tick Task 1's checkboxes above.

---

## Task 2: Serialization round-trip (mode survives reload)

**Files:**
- Modify: `ficsit-companion/tests/test_vehicle_station_node.cpp`

- [ ] **Step 1: Write the failing test** (append to the test file). This relies on the `Deserialize` routing added in Task 3, so it is expected to fail until Task 3 lands — write it now to drive that change:

```cpp
#include "domain/node_data_resolver.hpp"
#include "domain/json.hpp"
#include <unordered_map>

namespace
{
    class FakeResolver : public INodeDataResolver
    {
    public:
        std::unordered_map<std::string, const Item*> items;
        const Recipe* FindRecipe(const std::string&) const override { return nullptr; }
        const Item* FindItem(const std::string& n) const override
        {
            auto it = items.find(n); return it == items.end() ? nullptr : it->second;
        }
    };
}

/// @test An Unload-mode truck station re-serializes to identical JSON after a
///       Deserialize round-trip, proving `mode` and belt pins survive reload.
/// @covers VehicleStationNode::Serialize + Node::Deserialize routing.
TEST_CASE("VehicleStationNode round-trips with its mode", "[vehicle_station][serialization]")
{
    IdGen idgen;
    FakeResolver resolver;
    VehicleStationNode s(ax::NodeEditor::NodeId(idgen()),
        LogisticsNode::Kind::TruckStation, [&idgen] { return idgen(); });
    s.SetMode(VehicleStationNode::Mode::Unload, [&idgen] { return idgen(); });

    const Json::Value original = s.Serialize();
    std::unique_ptr<Node> rebuilt = Node::Deserialize(
        ax::NodeEditor::NodeId(idgen()), [&idgen] { return idgen(); }, original, resolver);

    REQUIRE(rebuilt->Serialize().Dump() == original.Dump());
    REQUIRE(static_cast<VehicleStationNode*>(rebuilt.get())->mode == VehicleStationNode::Mode::Unload);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ctest --test-dir build -C Release -R vehicle_station --output-on-failure`
Expected: FAIL — `Deserialize` still builds a plain `LogisticsNode` (no `mode`, downcast is UB / mismatched JSON). This drives Task 3.

- [ ] **Step 3:** No code here — proceed to Task 3 to make it pass. (Leave the test in place.)

---

## Task 3: Route Truck/Train `logistics_kind` to the subclass in `Deserialize`

**Files:**
- Modify: `ficsit-companion/src/domain/node_base.cpp:112-113`

- [ ] **Step 1: Change the `Kind::Logistics` case** in `Node::Deserialize` so vehicle stations build the subclass:

```cpp
    case Kind::Logistics:
    {
        const auto lk = static_cast<LogisticsNode::Kind>(serialized["logistics_kind"].get<int>());
        if (lk == LogisticsNode::Kind::TruckStation || lk == LogisticsNode::Kind::TrainStation)
        {
            return std::make_unique<VehicleStationNode>(id, id_generator, serialized, resolver);
        }
        return std::make_unique<LogisticsNode>(id, id_generator, serialized, resolver);
    }
```

- [ ] **Step 2: Run the round-trip test**

Run: `ctest --test-dir build -C Release -R vehicle_station --output-on-failure`
Expected: PASS (Task 2's test now green).

- [ ] **Step 3: Run the existing serialization suite** to confirm old saves still load:

Run: `ctest --test-dir build -C Release -R "serialization|node" --output-on-failure`
Expected: all PASS (the existing `Logistics node round-trips` test still passes because its node uses a non-station kind / the generic path).

- [ ] **Step 4: Record progress** (PROGRESS.md line + tick Tasks 2 & 3).

---

## Task 4: Pool discovery (`FindPool`)

**Files:**
- Create: `ficsit-companion/include/domain/vehicle_route.hpp`
- Create: `ficsit-companion/src/domain/vehicle_route.cpp`
- Modify: `ficsit-companion/tests/test_vehicle_station_node.cpp`

- [ ] **Step 1: Write the failing test** (append):

```cpp
#include "domain/vehicle_route.hpp"
#include "domain/link.hpp"
#include <vector>

namespace
{
    // Make a route (plug<->plug) link between a loader and an unloader and index
    // it on both stations. Returns the owning Link (kept alive by `storage`).
    Link* LinkPlugs(VehicleStationNode* loader, VehicleStationNode* unloader,
                    IdGen& idgen, std::vector<std::unique_ptr<Link>>& storage)
    {
        storage.emplace_back(std::make_unique<Link>(
            ax::NodeEditor::LinkId(idgen()), loader->plug.get(), unloader->plug.get()));
        Link* l = storage.back().get();
        loader->route_links.push_back(l);
        unloader->route_links.push_back(l);
        return l;
    }
}

/// @test FindPool returns every station reachable through route links (A,B,C->D
///       is one pool of four), regardless of which member you start from.
/// @covers VehicleRoute::FindPool connected-component traversal.
TEST_CASE("FindPool gathers the connected route component", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode B(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode C(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);
    LinkPlugs(&B, &D, idgen, storage);
    LinkPlugs(&C, &D, idgen, storage);

    std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(&D);
    REQUIRE(pool.size() == 4);

    std::vector<VehicleStationNode*> from_a = VehicleRoute::FindPool(&A);
    REQUIRE(from_a.size() == 4);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release --target fc-tests`
Expected: FAIL to compile — `vehicle_route.hpp` / `FindPool` missing.

- [ ] **Step 3: Implement the header** `include/domain/vehicle_route.hpp`:

```cpp
#pragma once

#include <vector>

#include "domain/fractional_number.hpp"

struct VehicleStationNode;
struct Item;

namespace VehicleRoute
{
    /// @brief All vehicle stations connected to `origin` through route links
    /// (the connected component), including `origin`. Order is unspecified.
    std::vector<VehicleStationNode*> FindPool(VehicleStationNode* origin);

    struct ItemBalance
    {
        const Item* item = nullptr;
        FractionalNumber supply{ 0, 1 }; ///< sum of loader cargo-input rates
        FractionalNumber demand{ 0, 1 }; ///< sum of unloader cargo-output rates
    };

    /// @brief Per-cargo-item supply (loaders) vs demand (unloaders) over a pool.
    std::vector<ItemBalance> SummarizePool(const std::vector<VehicleStationNode*>& pool);
}
```

- [ ] **Step 4: Implement** `src/domain/vehicle_route.cpp` (FindPool only for now; SummarizePool stub returns empty — filled in Task 5):

```cpp
#include "domain/vehicle_route.hpp"

#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"

#include <unordered_set>

namespace
{
    // The other plug's owning station for a route link, given one endpoint plug.
    VehicleStationNode* OtherStation(const Link* link, const VehicleStationNode* self)
    {
        if (link == nullptr) return nullptr;
        const Pin* other = (link->start != nullptr && link->start->node == self) ? link->end : link->start;
        if (other == nullptr || other->node == nullptr) return nullptr;
        Node* n = other->node;
        if (!n->IsLogistics()) return nullptr;
        LogisticsNode* l = static_cast<LogisticsNode*>(n);
        if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
            l->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
        return static_cast<VehicleStationNode*>(l);
    }
}

namespace VehicleRoute
{
    std::vector<VehicleStationNode*> FindPool(VehicleStationNode* origin)
    {
        std::vector<VehicleStationNode*> pool;
        if (origin == nullptr) return pool;
        std::unordered_set<VehicleStationNode*> seen;
        std::vector<VehicleStationNode*> queue{ origin };
        while (!queue.empty())
        {
            VehicleStationNode* n = queue.back();
            queue.pop_back();
            if (!seen.insert(n).second) continue;
            pool.push_back(n);
            for (const Link* l : n->route_links)
            {
                if (VehicleStationNode* o = OtherStation(l, n))
                {
                    if (seen.find(o) == seen.end()) queue.push_back(o);
                }
            }
        }
        return pool;
    }

    std::vector<ItemBalance> SummarizePool(const std::vector<VehicleStationNode*>&)
    {
        return {}; // Implemented in Task 5.
    }
}
```

- [ ] **Step 5: Build & run**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R route --output-on-failure`
Expected: `FindPool` test PASS.

- [ ] **Step 6: Record progress** (PROGRESS.md + tick Task 4).

---

## Task 5: Route summary (`SummarizePool`)

**Files:**
- Modify: `ficsit-companion/src/domain/vehicle_route.cpp`
- Modify: `ficsit-companion/tests/test_vehicle_station_node.cpp`

- [ ] **Step 1: Write the failing test** (append). Loaders A,B,C each feed 60 Rotor; D should demand the matching total:

```cpp
#include "domain/recipe.hpp" // Item

/// @test SummarizePool sums loader cargo-input rates as supply and unloader
///       cargo-output rates as demand, per item, across the pool.
/// @covers VehicleRoute::SummarizePool.
TEST_CASE("SummarizePool reports per-item supply vs demand", "[vehicle_station][route]")
{
    IdGen idgen;
    auto gen = [&idgen] { return idgen(); };
    Item rotor("Rotor", "", 100);

    VehicleStationNode A(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    VehicleStationNode D(ax::NodeEditor::NodeId(idgen()), LogisticsNode::Kind::TruckStation, gen);
    D.SetMode(VehicleStationNode::Mode::Unload, gen);

    // Loader A: first cargo input carries 60 Rotor/min.
    A.ins[0]->item = &rotor;
    A.ins[0]->current_rate = FractionalNumber(60, 1);
    // Unloader D: first cargo output carries 60 Rotor/min.
    D.outs[0]->item = &rotor;
    D.outs[0]->current_rate = FractionalNumber(60, 1);

    std::vector<std::unique_ptr<Link>> storage;
    LinkPlugs(&A, &D, idgen, storage);

    auto balances = VehicleRoute::SummarizePool(VehicleRoute::FindPool(&A));
    REQUIRE(balances.size() == 1);
    REQUIRE(balances[0].item == &rotor);
    REQUIRE(balances[0].supply == FractionalNumber(60, 1));
    REQUIRE(balances[0].demand == FractionalNumber(60, 1));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R route --output-on-failure`
Expected: FAIL — `SummarizePool` returns empty.

- [ ] **Step 3: Implement** `SummarizePool` in `vehicle_route.cpp` (replace the stub). Fuel inlet is the last input and must be excluded from supply:

```cpp
    std::vector<ItemBalance> SummarizePool(const std::vector<VehicleStationNode*>& pool)
    {
        // Preserve first-seen item order while accumulating.
        std::vector<ItemBalance> out;
        auto slot = [&](const Item* it) -> ItemBalance& {
            for (auto& b : out) if (b.item == it) return b;
            out.push_back(ItemBalance{ it, FractionalNumber(0,1), FractionalNumber(0,1) });
            return out.back();
        };
        for (VehicleStationNode* s : pool)
        {
            if (s->mode == VehicleStationNode::Mode::Load)
            {
                // Cargo inputs only: skip the fuel inlet (last input).
                for (size_t i = 0; i + 1 < s->ins.size(); ++i)
                {
                    const Pin* p = s->ins[i].get();
                    if (p->item != nullptr) slot(p->item).supply += p->current_rate;
                }
            }
            else
            {
                for (const auto& p : s->outs)
                {
                    if (p->item != nullptr) slot(p->item).demand += p->current_rate;
                }
            }
        }
        return out;
    }
```

- [ ] **Step 4: Build & run**

Run: `cmake --build build --config Release --target fc-tests && ctest --test-dir build -C Release -R route --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Record progress** (PROGRESS.md + tick Task 5).

---

## Task 6: GraphModel — create/delete route (plug) links + node-delete cleanup

**Files:**
- Modify: `ficsit-companion/src/domain/graph_model.cpp` (`CreateLink` ~53, `DeleteLink` ~168, `DeleteNode` ~213)
- Modify: `ficsit-companion/include/domain/graph_model.hpp` (declare the helper if you add one)

A route link is a link where **both** endpoints are plug pins. Detect it with a helper.

- [ ] **Step 1: Add a free helper** near the top of `graph_model.cpp` (anonymous namespace):

```cpp
namespace
{
    bool IsPlugPin(const Pin* p)
    {
        if (p == nullptr || p->node == nullptr || !p->node->IsLogistics()) return false;
        LogisticsNode* l = static_cast<LogisticsNode*>(p->node);
        if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
            l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
        return static_cast<VehicleStationNode*>(l)->plug.get() == p;
    }
}
```

- [ ] **Step 2: In `GraphModel::CreateLink`**, after computing `real_start`/`real_end` and emplacing the `Link`, branch for plug links so they index into `route_links` instead of `Pin::link` and skip rate propagation:

```cpp
    links.emplace_back(std::make_unique<Link>(GetNextId(), real_start, real_end));
    Link* created = links.back().get();
    if (IsPlugPin(real_start) && IsPlugPin(real_end))
    {
        // Route link: tracked per-station, invisible to the solver. Leave Pin::link null.
        static_cast<VehicleStationNode*>(real_start->node)->route_links.push_back(created);
        static_cast<VehicleStationNode*>(real_end->node)->route_links.push_back(created);
        return;
    }
    start->link = created;
    end->link = created;
    // ... existing rate-propagation block unchanged ...
```

- [ ] **Step 3: In `GraphModel::DeleteLink`**, before the existing `start->link = nullptr` / `end->link = nullptr` handling, detect a route link and unindex it instead:

```cpp
    if (it != links.end())
    {
        Link* l = it->get();
        if (IsPlugPin(l->start) && IsPlugPin(l->end))
        {
            auto unindex = [&](Pin* plug) {
                auto* v = static_cast<VehicleStationNode*>(plug->node);
                auto& rl = v->route_links;
                rl.erase(std::remove(rl.begin(), rl.end(), l), rl.end());
            };
            unindex(l->start);
            unindex(l->end);
            links.erase(it);
            return;
        }
        // ... existing non-route handling unchanged ...
    }
```

(Add `#include <algorithm>` if not present.)

- [ ] **Step 4: In `GraphModel::DeleteNode`**, after deleting links on `ins`/`outs`, also delete the plug's route links so they don't dangle. Insert after the `outs` loop:

```cpp
        if ((*it)->IsLogistics())
        {
            LogisticsNode* l = static_cast<LogisticsNode*>(it->get());
            if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                l->logistics_kind == LogisticsNode::Kind::TrainStation)
            {
                auto* v = static_cast<VehicleStationNode*>(l);
                // Copy ids first: DeleteLink mutates route_links.
                std::vector<ax::NodeEditor::LinkId> ids;
                for (Link* rl : v->route_links) ids.push_back(rl->id);
                for (auto id : ids) DeleteLink(id);
            }
        }
```

- [ ] **Step 5: Build the full app + tests** (this is UI-graph plumbing; verified via build now and manual test in Task 11):

Run: `cmake --build build --config Release`
Expected: links and the whole app compile.

- [ ] **Step 6: Run the full test suite** to confirm no regressions in graph/link behavior:

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all PASS.

- [ ] **Step 7: Record progress** (PROGRESS.md + tick Task 6).

---

## Task 7: production_app — create the subclass, find the plug pin, serialize routes

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp` (menu create ~3091; `FindPin`; session save/load of route links)

- [ ] **Step 1: Menu creation.** Replace the `TruckStation` / `TrainStation` create lines (~3091-3096) to build the subclass:

```cpp
            case RecipeSelectionIndex::TruckStation:
                nodes.emplace_back(std::make_unique<VehicleStationNode>(GetNextId(), LogisticsNode::Kind::TruckStation, std::bind(&ProductionApp::GetNextId, this)));
                break;
            case RecipeSelectionIndex::TrainStation:
                nodes.emplace_back(std::make_unique<VehicleStationNode>(GetNextId(), LogisticsNode::Kind::TrainStation, std::bind(&ProductionApp::GetNextId, this)));
                break;
```

- [ ] **Step 2: `FindPin` must see plug pins.** Locate `ProductionApp::FindPin` (grep `Pin* ProductionApp::FindPin`). In its node loop, after scanning `ins`/`outs`, add:

```cpp
        if (node->IsLogistics())
        {
            auto* l = static_cast<LogisticsNode*>(node.get());
            if (l->logistics_kind == LogisticsNode::Kind::TruckStation ||
                l->logistics_kind == LogisticsNode::Kind::TrainStation)
            {
                Pin* plug = static_cast<VehicleStationNode*>(l)->plug.get();
                if (plug != nullptr && plug->id == id) return plug;
            }
        }
```

- [ ] **Step 3: Persist route links.** Find where the session is serialized (grep for where `links` are written, likely `Serialize`/session JSON in `production_app.cpp` or `session_serializer`). Route links are NOT in `Pin::link`, so the normal link serializer that walks `pin->link` will miss them. Add a `route_links` array to the saved session listing `{ start_node, end_node }` plug pairs (use node ids; each station has exactly one plug). On load, after all nodes are deserialized, recreate each route link via `GraphModel::CreateLink(stationA->plug.get(), stationB->plug.get(), false, ...)`.

Concretely, in the session save loop, after writing nodes+links, add:

```cpp
    Json::Array route_links_json;
    for (const auto& n : nodes)
    {
        if (!n->IsLogistics()) continue;
        auto* l = static_cast<LogisticsNode*>(n.get());
        if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
            l->logistics_kind != LogisticsNode::Kind::TrainStation) continue;
        auto* v = static_cast<VehicleStationNode*>(l);
        for (const Link* rl : v->route_links)
        {
            // Write each link once: only from its output (Load) end.
            if (rl->start != v->plug.get()) continue;
            route_links_json.push_back({
                { "from", static_cast<long long>(rl->start->node->id.Get()) },
                { "to",   static_cast<long long>(rl->end->node->id.Get()) },
            });
        }
    }
    session["route_links"] = route_links_json;
```

On load, after nodes exist (and node ids are mapped), iterate `session["route_links"]` and connect the two stations' plugs. Match the existing load code's id-remap approach (the loader already maps saved node ids to live nodes — reuse that map).

- [ ] **Step 4: Build**

Run: `cmake --build build --config Release`
Expected: compiles.

- [ ] **Step 5: Record progress** (PROGRESS.md + tick Task 7). Route-link save/load is verified manually in Task 11.

---

## Task 8: production_app — render the plug (distinct color) + Load/Unload toggle

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp` (node rendering for Logistics, ~2620; the per-node pin rendering loop)

- [ ] **Step 1:** In the logistics-node rendering branch (where `GetDisplayName()` is shown, ~2623), for a `VehicleStationNode` add a Load/Unload toggle in the node header:

```cpp
            if (logistics_node->logistics_kind == LogisticsNode::Kind::TruckStation ||
                logistics_node->logistics_kind == LogisticsNode::Kind::TrainStation)
            {
                auto* v = static_cast<VehicleStationNode*>(const_cast<LogisticsNode*>(logistics_node));
                int m = static_cast<int>(v->mode);
                ImGui::SetNextItemWidth(ImGui::GetTextLineHeight() * 7.0f);
                if (ImGui::RadioButton("Load", m == 0)) v->SetMode(VehicleStationNode::Mode::Load, std::bind(&ProductionApp::GetNextId, this));
                ImGui::SameLine();
                if (ImGui::RadioButton("Unload", m == 1)) v->SetMode(VehicleStationNode::Mode::Unload, std::bind(&ProductionApp::GetNextId, this));
            }
```

> Because `SetMode` drops route links, also delete the underlying `Link` objects when the mode actually changes. Wrap the radio handlers to first collect `v->route_links` ids and call `DeleteLink` on each, THEN call `SetMode`. (Mirror the cleanup pattern from Task 6 Step 4.)

- [ ] **Step 2:** Render the plug pin. After the loop that renders `outs` pins for the node, render the plug on the appropriate side using the node editor pin API used for other pins (grep how `ins`/`outs` pins call `ax::NodeEditor::BeginPin`/`EndPin` in this file and mirror it). Draw it with a distinct color, e.g. `ImColor(255, 170, 0)` (amber), vs. the belt pins. The plug's `direction` already dictates Input (left) vs Output (right) placement.

- [ ] **Step 3: Build & launch the app** to verify rendering:

Run: `cmake --build build --config Release`
Then run the built app (see `CLAUDE.md` build/run section) and confirm: placing a Truck Station shows 3 in / 2 out belts, an amber plug, and a working Load/Unload toggle that moves the plug from the right (Load) to the left (Unload).

- [ ] **Step 4: Record progress** (PROGRESS.md + tick Task 8).

---

## Task 9: production_app — plug link-creation rules

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp` (link query/accept block, ~2718-2747)

- [ ] **Step 1:** Extend the link validation. Add a `start_is_plug`/`end_is_plug` check (reuse an `IsPlugPin` helper — promote the one from Task 6 to a shared header `domain/vehicle_route.hpp` or `graph_item_resolve.hpp` and include it). The rules:
  - If exactly one end is a plug → reject (no belt↔plug).
  - If both ends are plugs → **bypass** the `start_pin->link != nullptr || end_pin->link != nullptr` rejection (plugs are multi-link) and the item-match rejection (plugs are untyped). Still require opposite directions (`Output`→`Input`, already enforced) and different nodes.

Concretely, compute `const bool both_plugs = IsPlugPin(start_pin) && IsPlugPin(end_pin);` and `const bool one_plug = IsPlugPin(start_pin) != IsPlugPin(end_pin);` then adjust the big `if (...)` reject condition:

```cpp
                if (start_pin == nullptr ||
                    end_pin == nullptr ||
                    start_pin == end_pin ||
                    start_pin->direction == end_pin->direction ||
                    start_pin->node == end_pin->node ||
                    one_plug ||                                  // belt<->plug not allowed
                    (!both_plugs && (start_pin->link != nullptr || end_pin->link != nullptr)) ||
                    (!both_plugs && start_pin->item != nullptr && end_pin->item != nullptr && start_pin->item != end_pin->item) ||
                    (!both_plugs && start_pin->GetLocked() && end_pin->GetLocked() && start_pin->current_rate != end_pin->current_rate) ||
                    fuel_pin_rejects
                )
```

- [ ] **Step 2: Build & launch.** Verify: you can draw a plug→plug link from a Load station to an Unload station; you can draw a *second* plug link into the same Unload plug (multi-link); belt→plug is rejected; plug→plug between two Load stations is rejected (same direction).

Run: `cmake --build build --config Release` then exercise in the app.

- [ ] **Step 3: Record progress** (PROGRESS.md + tick Task 9).

---

## Task 10: production_app — route summary panel

**Files:**
- Modify: `ficsit-companion/src/app/production_app.cpp` (node detail / side panel rendering)

- [ ] **Step 1:** When a single `VehicleStationNode` is selected (or hovered), compute `VehicleRoute::SummarizePool(VehicleRoute::FindPool(node))` and render a small table: one row per item with `supply`, `demand`, and a mismatch flag (`supply != demand` → red text). Place it near the existing per-node detail rendering. Use the `FractionalNumber` string formatting already used elsewhere for rates (grep how rates are displayed, e.g. `GetStringFraction`/`.GetNumerator()` formatting).

```cpp
        // inside the selected-node detail block, for a VehicleStationNode v:
        std::vector<VehicleStationNode*> pool = VehicleRoute::FindPool(v);
        auto balances = VehicleRoute::SummarizePool(pool);
        ImGui::TextUnformatted("Route balance:");
        for (const auto& b : balances)
        {
            const bool mismatch = !(b.supply == b.demand);
            if (mismatch) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.4f,0.4f,1));
            ImGui::Text("%s: %s in -> %s out",
                b.item ? b.item->name.c_str() : "?",
                b.supply.GetStringFraction().c_str(),
                b.demand.GetStringFraction().c_str());
            if (mismatch) ImGui::PopStyleColor();
        }
```

(Adjust `GetStringFraction` to the actual `FractionalNumber` formatting method — grep `FractionalNumber` usage in `production_app.cpp`.)

- [ ] **Step 2: Build & launch.** Build the A,B,C→D example by hand: three loaders feeding Rotor into one unloader, plug-linked. Confirm the panel shows `Rotor: <A+B+C> in -> <D> out` and flags red when they differ.

Run: `cmake --build build --config Release` then verify in the app.

- [ ] **Step 3: Record progress** (PROGRESS.md + tick Task 10).

---

## Task 11: Full verification

**Files:** none (verification only)

- [ ] **Step 1: Full test suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: all tests PASS, including the new `[vehicle_station]` cases and all pre-existing suites.

- [ ] **Step 2: Manual end-to-end** in the running app:
  - Add 4 Truck Stations; set A,B,C = Load, D = Unload.
  - Wire a Rotor source into each loader's cargo input; wire D's cargo output to a consumer.
  - Plug-link A,B,C → D (three links into D's plug).
  - Confirm the route-summary panel shows supply vs demand for Rotor.
  - Save the session, reload it, and confirm the stations, modes, belts, and the three plug links all come back (Task 7 route-link persistence).
  - Delete station B; confirm its plug links vanish and no crash.

- [ ] **Step 3: Update spec status & PROGRESS.** Append the final PROGRESS line and note Phase 1 complete. Phase 2 (solver auto-balance) is a separate plan.

---

## Self-Review notes (spec coverage)

- Spec §3 belt layout / mode → Tasks 1, 8. (Belts pre-existed; reused.)
- Spec §4 plug, multi-link, plug↔plug-only, pool → Tasks 1, 4, 6, 9.
- Spec §6 serialization + route persistence → Tasks 2, 3, 7.
- Spec §7 UI menu/toggle/plug color/summary → Tasks 7, 8, 9, 10.
- Spec §8 testing → Tasks 1, 2, 4, 5, 11.
- Spec §9 train reuse → subclass is kind-agnostic; menu wires both (Task 7).
- Spec §5 demand auto-balance is **Phase 2** — intentionally out of this plan.
