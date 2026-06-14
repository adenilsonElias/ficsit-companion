# Resource Flow Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Resource Flow tool that scans the in-memory node graph and reports, per item, gross produced vs consumed and a deficit/surplus/balanced status, shown in a toggleable floating ImGui window.

**Architecture:** A pure `fc-core` domain module (`domain/resource_flow.{hpp,cpp}`) computes a `ResourceFlowReport` from `std::vector<std::unique_ptr<Node>>` and exposes a pure filter predicate — both covered by Catch2 tests. `ProductionApp` adds a `RenderResourceFlowWindow()` that rebuilds the report each frame and draws the table; a left-panel button toggles it; the open flag persists in `settings.json`. This mirrors the existing Vehicle Map split (pure `VehicleMapQuery` + app-layer rendering).

**Tech Stack:** C++17, Dear ImGui + ImGui Node Editor, `FractionalNumber` (exact rationals), Catch2, CMake.

---

## Background the implementer needs

- **Rates live on pins.** `Pin` (`include/domain/pin.hpp`) has `const Item* item` and `FractionalNumber current_rate`. Producers' `outs` and consumers' `ins` carry the per-minute rate.
- **Node kinds are virtual predicates** on `Node` (`include/domain/node.hpp`): `IsCraft()`, `IsExtractor()`, `IsSink()`, `IsGroup()`, `IsOrganizer()`, `IsLogistics()`. `GroupNode` (a `PoweredNode`) owns `std::vector<std::unique_ptr<Node>> nodes`.
- **Freshly constructed nodes have pin rates of 0.** A `CraftNode`'s pins are set by `UpdateRate(rate)` (`current_rate` defaults to 0, so call `UpdateRate(FractionalNumber(1,1))` to get rate-1 quantities). `ExtractorNode` likewise; its base rates are hardcoded (no game-data needed) and constructing one headless is safe (see `tests/test_node_serialization.cpp`), but to avoid its power path in tests we set the output pin's `current_rate` directly.
- **`ItemPtrCompare`** (`include/app/utils.hpp`, defined in `src/app/utils.cpp`) orders `const Item*` by `item->name`. `utils.cpp` is already linked into `fc-tests`. Using it as a `std::map` comparator yields name-sorted iteration for free.
- **`FractionalNumber`** (`include/domain/fractional_number.hpp`) default-constructs to `0/1`, supports `+= -= == < >`, free `operator-`, and `GetStringFloat()` / `GetValue()` for display.
- **Item display:** icons are drawn via `ImGui::Image((void*)(intptr_t)item->icon_gl_index, ImVec2(h, h))` where `h = ImGui::GetTextLineHeightWithSpacing()`; `item->name` is the display string.
- **Settings toggle pattern** (`src/app/production_app.cpp`): `if (ImGui::Checkbox("...", &settings.x)) { settings_store->Save(settings); }`.

## Build & test commands (Windows / PowerShell)

Assumes a configured `build/` dir. If not configured yet:
```
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
```
Build just the tests:
```
cmake --build build --config Release --target fc-tests
```
Run only this feature's tests (fast TDD loop) — the Catch2 exe lands here:
```
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Full suite:
```
ctest --test-dir build -C Release --output-on-failure
```
Build the app to eyeball the window:
```
cmake --build build --config Release --target ficsit-companion
```

## File structure

| File | Responsibility |
| --- | --- |
| `ficsit-companion/include/domain/resource_flow.hpp` (create) | Report/row/status/filter types + `BuildResourceFlowReport` + `RowPassesFilter` declarations |
| `ficsit-companion/src/domain/resource_flow.cpp` (create) | Graph walk, accumulation, classification, sorting, filter predicate |
| `ficsit-companion/tests/test_resource_flow.cpp` (create) | Catch2 tests for the calculation + filter |
| `ficsit-companion/CMakeLists.txt` (modify) | Register header, core source, test source |
| `ficsit-companion/include/infra/settings_store.hpp` (modify) | Add `bool show_resource_flow` to `Settings` |
| `ficsit-companion/src/infra/settings_store.cpp` (modify) | Load/save the `show_resource_flow` key |
| `ficsit-companion/tests/test_settings_store.cpp` (modify) | Cover the new setting round-trip |
| `ficsit-companion/include/app/production_app.hpp` (modify) | Window state fields + `RenderResourceFlowWindow()` decl |
| `ficsit-companion/src/app/production_app.cpp` (modify) | Window rendering, left-panel toggle button, render-loop call |

---

## Task 1: Domain module + craft accumulation

**Files:**
- Create: `ficsit-companion/include/domain/resource_flow.hpp`
- Create: `ficsit-companion/src/domain/resource_flow.cpp`
- Create: `ficsit-companion/tests/test_resource_flow.cpp`
- Modify: `ficsit-companion/CMakeLists.txt`

- [ ] **Step 1: Create the header**

`ficsit-companion/include/domain/resource_flow.hpp`:
```cpp
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "domain/fractional_number.hpp"

struct Item;
struct Node;

/// @brief Per-item balance classification.
enum class ResourceFlowStatus { Deficit, Balanced, Surplus };

/// @brief One item's gross produced/consumed totals across the whole graph.
struct ResourceFlowRow
{
    const Item* item = nullptr;
    FractionalNumber produced;          // defaults to 0/1
    FractionalNumber consumed;          // defaults to 0/1
    FractionalNumber net;               // produced - consumed
    ResourceFlowStatus status = ResourceFlowStatus::Balanced;
};

/// @brief Full report: rows (sorted by item name) plus tallies.
struct ResourceFlowReport
{
    std::vector<ResourceFlowRow> rows;
    std::size_t skipped_null_item_pins = 0;
    std::size_t surplus_count = 0;
    std::size_t deficit_count = 0;
    std::size_t balanced_count = 0;
};

/// @brief Scan the in-memory graph and build the per-item resource report.
/// Craft inputs/outputs, extractor outputs and sink inputs are tallied;
/// groups recurse into their subnodes (gross); organizers and logistics
/// contribute nothing. Pins with a null item are skipped and counted.
ResourceFlowReport BuildResourceFlowReport(const std::vector<std::unique_ptr<Node>>& nodes);

/// @brief Table filter mode.
enum class ResourceFlowFilter { All, Deficit, Surplus };

/// @brief True if a row should be shown under the given status filter and
/// case-insensitive name search (empty search matches all).
bool RowPassesFilter(const ResourceFlowRow& row, ResourceFlowFilter filter, std::string_view search);
```

- [ ] **Step 2: Create the source with the craft branch only**

`ficsit-companion/src/domain/resource_flow.cpp`:
```cpp
#include "domain/resource_flow.hpp"

#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"   // Item definition
#include "app/utils.hpp"       // ItemPtrCompare

#include <map>

namespace
{
    struct Agg
    {
        FractionalNumber produced;
        FractionalNumber consumed;
    };

    using Totals = std::map<const Item*, Agg, ItemPtrCompare>;

    void AccumulatePins(const std::vector<std::unique_ptr<Pin>>& pins, const bool as_produced,
                        Totals& totals, std::size_t& skipped)
    {
        for (const auto& pin : pins)
        {
            if (pin->item == nullptr)
            {
                ++skipped;
                continue;
            }
            Agg& agg = totals[pin->item];
            if (as_produced)
            {
                agg.produced += pin->current_rate;
            }
            else
            {
                agg.consumed += pin->current_rate;
            }
        }
    }

    void Walk(const std::vector<std::unique_ptr<Node>>& nodes, Totals& totals, std::size_t& skipped)
    {
        for (const auto& node : nodes)
        {
            if (node->IsCraft())
            {
                AccumulatePins(node->ins, false, totals, skipped);
                AccumulatePins(node->outs, true, totals, skipped);
            }
            // Extractor / Sink / Group branches added in later tasks.
            // Organizer / Logistics contribute nothing.
        }
    }
}

ResourceFlowReport BuildResourceFlowReport(const std::vector<std::unique_ptr<Node>>& nodes)
{
    Totals totals;
    ResourceFlowReport report;

    Walk(nodes, totals, report.skipped_null_item_pins);

    const FractionalNumber zero(0, 1);
    for (const auto& [item, agg] : totals) // ItemPtrCompare => sorted by name
    {
        ResourceFlowRow row;
        row.item = item;
        row.produced = agg.produced;
        row.consumed = agg.consumed;
        row.net = agg.produced - agg.consumed;

        if (row.net > zero)
        {
            row.status = ResourceFlowStatus::Surplus;
            ++report.surplus_count;
        }
        else if (row.net < zero)
        {
            row.status = ResourceFlowStatus::Deficit;
            ++report.deficit_count;
        }
        else
        {
            row.status = ResourceFlowStatus::Balanced;
            ++report.balanced_count;
        }
        report.rows.push_back(row);
    }

    return report;
}

bool RowPassesFilter(const ResourceFlowRow&, ResourceFlowFilter, std::string_view)
{
    return true; // Real implementation in Task 6.
}
```

- [ ] **Step 3: Wire CMake**

In `ficsit-companion/CMakeLists.txt`:
- Add to `HEADER_FILES` (after `include/domain/recipe.hpp`):
  ```
  	include/domain/resource_flow.hpp
  ```
- Add to `DOMAIN_SOURCE_FILES` (after `src/domain/recipe.cpp`):
  ```
      src/domain/resource_flow.cpp
  ```
- Add to `TEST_SOURCE_FILES` (after `tests/test_group_node.cpp`):
  ```
          tests/test_resource_flow.cpp
  ```

- [ ] **Step 4: Write the failing test**

`ficsit-companion/tests/test_resource_flow.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "domain/building.hpp"
#include "domain/fractional_number.hpp"
#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"
#include "domain/resource_flow.hpp"

#include "graph_test_helpers.hpp" // IdGen

namespace
{
    // 30 ore -> 20 plate, and 20 plate -> 10 rod, in a dummy building (no game
    // data needed). Holds fixture data so items/recipes outlive the nodes.
    struct FlowFixture
    {
        IdGen idgen;
        Building building{ "Test_Constructor", FractionalNumber(0, 1), 4.0, 1.6, 2.0, false };
        Item ore{ "Iron Ore", "", 1 };
        Item plate{ "Iron Plate", "", 2 };
        Item rod{ "Iron Rod", "", 3 };

        std::vector<CountedItem> plate_in{ CountedItem(&ore, FractionalNumber(30, 1)) };
        std::vector<CountedItem> plate_out{ CountedItem(&plate, FractionalNumber(20, 1)) };
        Recipe plate_recipe{ plate_in, plate_out, &building, false, 4.0, "Recipe_IronPlate_C" };

        std::vector<CountedItem> rod_in{ CountedItem(&plate, FractionalNumber(20, 1)) };
        std::vector<CountedItem> rod_out{ CountedItem(&rod, FractionalNumber(10, 1)) };
        Recipe rod_recipe{ rod_in, rod_out, &building, false, 4.0, "Recipe_IronRod_C" };

        std::unique_ptr<CraftNode> MakeCraft(const Recipe& r, const FractionalNumber& rate)
        {
            auto craft = std::make_unique<CraftNode>(
                ax::NodeEditor::NodeId(idgen()), &r, [this] { return idgen(); });
            craft->UpdateRate(rate);
            return craft;
        }

        const ResourceFlowRow* Find(const ResourceFlowReport& rep, const Item* item)
        {
            for (const auto& row : rep.rows)
            {
                if (row.item == item) return &row;
            }
            return nullptr;
        }
    };
}

/// @test   A single craft at rate 1 reports its recipe inputs as consumed and
///         outputs as produced: ore 30 consumed (deficit), plate 20 produced
///         (surplus); rows are name-sorted, counts tally, and nothing is skipped.
/// @covers BuildResourceFlowReport craft branch, status classification, counts,
///         and ItemPtrCompare-based row ordering.
TEST_CASE("single craft reports consumed inputs and produced outputs", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.size() == 2);
    REQUIRE(rep.skipped_null_item_pins == 0);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(ore != nullptr);
    REQUIRE(plate != nullptr);

    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(ore->produced == FractionalNumber(0, 1));
    REQUIRE(ore->net == FractionalNumber(-30, 1));
    REQUIRE(ore->status == ResourceFlowStatus::Deficit);

    REQUIRE(plate->produced == FractionalNumber(20, 1));
    REQUIRE(plate->consumed == FractionalNumber(0, 1));
    REQUIRE(plate->net == FractionalNumber(20, 1));
    REQUIRE(plate->status == ResourceFlowStatus::Surplus);

    // Rows sorted by name: "Iron Ore" before "Iron Plate".
    REQUIRE(rep.rows[0].item == &fx.ore);
    REQUIRE(rep.rows[1].item == &fx.plate);

    REQUIRE(rep.deficit_count == 1);
    REQUIRE(rep.surplus_count == 1);
    REQUIRE(rep.balanced_count == 0);
}
```

- [ ] **Step 5: Build and run — expect PASS**

The implementation in Step 2 already satisfies this test (craft branch is complete). Run:
```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: `All tests passed` (1 test case). If the build fails to find the new files, re-run the configure command from "Build & test commands".

- [ ] **Step 6: Commit**

```
git add ficsit-companion/include/domain/resource_flow.hpp ficsit-companion/src/domain/resource_flow.cpp ficsit-companion/tests/test_resource_flow.cpp ficsit-companion/CMakeLists.txt
git commit -m "feat(resource-flow): domain module with craft accumulation"
```

---

## Task 2: Extractor production

**Files:**
- Modify: `ficsit-companion/src/domain/resource_flow.cpp`
- Modify: `ficsit-companion/tests/test_resource_flow.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_resource_flow.cpp`:
```cpp
/// @test   An extractor's output is counted as production and offsets a craft's
///         raw-resource deficit: a miner producing 30 ore against a craft
///         consuming 30 ore nets the ore to 0 (balanced).
/// @covers BuildResourceFlowReport extractor branch (outs counted as produced).
TEST_CASE("extractor output offsets a craft's raw-resource deficit", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1))); // consumes 30 ore

    auto extractor = std::make_unique<ExtractorNode>(
        ax::NodeEditor::NodeId(fx.idgen()), ExtractorNode::Kind::MinerMk2, &fx.ore,
        ExtractorNode::Purity::Normal, [&fx] { return fx.idgen(); });
    extractor->outs[0]->current_rate = FractionalNumber(30, 1); // set directly, skip power path
    nodes.push_back(std::move(extractor));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    REQUIRE(ore != nullptr);
    REQUIRE(ore->produced == FractionalNumber(30, 1));
    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(ore->net == FractionalNumber(0, 1));
    REQUIRE(ore->status == ResourceFlowStatus::Balanced);
}
```

- [ ] **Step 2: Run — expect FAIL**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: the new test FAILS — ore `produced` is `0/1` (extractor not yet counted), status `Deficit`.

- [ ] **Step 3: Add the extractor branch**

In `src/domain/resource_flow.cpp`, inside `Walk`, after the `IsCraft()` block:
```cpp
            else if (node->IsExtractor())
            {
                AccumulatePins(node->outs, true, totals, skipped);
            }
```

- [ ] **Step 4: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: all `[resource_flow]` tests pass (2 cases).

- [ ] **Step 5: Commit**

```
git add ficsit-companion/src/domain/resource_flow.cpp ficsit-companion/tests/test_resource_flow.cpp
git commit -m "feat(resource-flow): count extractor output as production"
```

---

## Task 3: Sink consumption + null-item skip

**Files:**
- Modify: `ficsit-companion/src/domain/resource_flow.cpp`
- Modify: `ficsit-companion/tests/test_resource_flow.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_resource_flow.cpp`:
```cpp
/// @test   A sink's input is counted as consumption: a sink consuming 20 plate
///         (with nothing producing it) reports plate as a 20/min deficit.
/// @covers BuildResourceFlowReport sink branch (ins counted as consumed).
TEST_CASE("sink input is counted as consumption", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, &fx.plate);
    sink->ins[0]->current_rate = FractionalNumber(20, 1);
    nodes.push_back(std::move(sink));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(plate != nullptr);
    REQUIRE(plate->consumed == FractionalNumber(20, 1));
    REQUIRE(plate->net == FractionalNumber(-20, 1));
    REQUIRE(plate->status == ResourceFlowStatus::Deficit);
}

/// @test   Inspected pins with a null item are not tallied but are counted in
///         skipped_null_item_pins (here: a sink created with no item).
/// @covers BuildResourceFlowReport null-item handling in AccumulatePins.
TEST_CASE("null-item pins are skipped and counted", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    auto sink = std::make_unique<SinkNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, nullptr);
    sink->ins[0]->current_rate = FractionalNumber(5, 1);
    nodes.push_back(std::move(sink));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.empty());
    REQUIRE(rep.skipped_null_item_pins == 1);
}
```

- [ ] **Step 2: Run — expect FAIL**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: both new tests FAIL — sink inputs are not yet inspected, so plate has no row and `skipped_null_item_pins` stays 0.

- [ ] **Step 3: Add the sink branch**

In `src/domain/resource_flow.cpp`, inside `Walk`, after the `IsExtractor()` block:
```cpp
            else if (node->IsSink())
            {
                AccumulatePins(node->ins, false, totals, skipped);
            }
```

- [ ] **Step 4: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: all `[resource_flow]` tests pass (4 cases).

- [ ] **Step 5: Commit**

```
git add ficsit-companion/src/domain/resource_flow.cpp ficsit-companion/tests/test_resource_flow.cpp
git commit -m "feat(resource-flow): count sink input as consumption, skip null items"
```

---

## Task 4: Group recursion

**Files:**
- Modify: `ficsit-companion/src/domain/resource_flow.cpp`
- Modify: `ficsit-companion/tests/test_resource_flow.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_resource_flow.cpp` (note the extra include at the top of the file is already present via node.hpp; `Link` is needed for the group ctor):
```cpp
/// @test   Group contents are analyzed recursively (gross): a group wrapping a
///         rate-1 craft contributes the craft's 30 ore consumed and 20 plate
///         produced to the report, exactly as if the craft were top-level.
/// @covers BuildResourceFlowReport group branch (recurse into group->nodes).
TEST_CASE("group contents are analyzed recursively", "[resource_flow]")
{
    FlowFixture fx;

    std::vector<std::unique_ptr<Node>> subnodes;
    subnodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));
    std::vector<std::unique_ptr<Link>> sublinks;

    auto group = std::make_unique<GroupNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); },
        std::move(subnodes), std::move(sublinks));
    group->UpdateRate(FractionalNumber(1, 1));

    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::move(group));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* ore = fx.Find(rep, &fx.ore);
    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(ore != nullptr);
    REQUIRE(plate != nullptr);
    REQUIRE(ore->consumed == FractionalNumber(30, 1));
    REQUIRE(plate->produced == FractionalNumber(20, 1));
}
```
Add `#include "domain/link.hpp"` to the include block at the top of `test_resource_flow.cpp`.

- [ ] **Step 2: Run — expect FAIL**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: the group test FAILS — the group is neither craft/extractor/sink, so its contents are ignored and the report is empty.

- [ ] **Step 3: Add the group branch**

In `src/domain/resource_flow.cpp`, inside `Walk`, after the `IsSink()` block:
```cpp
            else if (node->IsGroup())
            {
                const GroupNode* group = static_cast<const GroupNode*>(node.get());
                Walk(group->nodes, totals, skipped);
            }
```

- [ ] **Step 4: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: all `[resource_flow]` tests pass (5 cases).

- [ ] **Step 5: Commit**

```
git add ficsit-companion/src/domain/resource_flow.cpp ficsit-companion/tests/test_resource_flow.cpp
git commit -m "feat(resource-flow): recurse into group nodes (gross)"
```

---

## Task 5: Robustness — organizers/logistics excluded, fractional exactness

No production code changes; these characterize behavior the walk already has (organizers/logistics fall through; `FractionalNumber` is exact).

**Files:**
- Modify: `ficsit-companion/tests/test_resource_flow.cpp`

- [ ] **Step 1: Write the tests**

Append to `tests/test_resource_flow.cpp`:
```cpp
/// @test   Organizer and logistics nodes create no production or consumption: a
///         graph of only a merger and a truck station yields an empty report
///         with nothing skipped (their pins are never inspected).
/// @covers BuildResourceFlowReport exclusion of IsOrganizer()/IsLogistics().
TEST_CASE("organizers and logistics contribute nothing", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;

    nodes.push_back(std::make_unique<MergerNode>(
        ax::NodeEditor::NodeId(fx.idgen()), [&fx] { return fx.idgen(); }, &fx.plate));
    nodes.push_back(std::make_unique<LogisticsNode>(
        ax::NodeEditor::NodeId(fx.idgen()), LogisticsNode::Kind::TruckStation, 1, 1,
        [&fx] { return fx.idgen(); }));

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    REQUIRE(rep.rows.empty());
    REQUIRE(rep.skipped_null_item_pins == 0);
}

/// @test   Fractional rates are preserved exactly: a craft producing 20 plate
///         against another consuming 20/3 plate nets plate to exactly 40/3.
/// @covers BuildResourceFlowReport exact-rational accumulation (no float drift).
TEST_CASE("fractional rates preserve exact net values", "[resource_flow]")
{
    FlowFixture fx;
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(fx.MakeCraft(fx.plate_recipe, FractionalNumber(1, 1)));        // +20 plate
    nodes.push_back(fx.MakeCraft(fx.rod_recipe, FractionalNumber(1, 3)));          // consumes 20*(1/3)=20/3 plate

    const ResourceFlowReport rep = BuildResourceFlowReport(nodes);

    const ResourceFlowRow* plate = fx.Find(rep, &fx.plate);
    REQUIRE(plate != nullptr);
    REQUIRE(plate->produced == FractionalNumber(20, 1));
    REQUIRE(plate->consumed == FractionalNumber(20, 3));
    REQUIRE(plate->net == FractionalNumber(40, 3));
    REQUIRE(plate->status == ResourceFlowStatus::Surplus);
}
```

- [ ] **Step 2: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: all `[resource_flow]` tests pass (7 cases).

- [ ] **Step 3: Commit**

```
git add ficsit-companion/tests/test_resource_flow.cpp
git commit -m "test(resource-flow): organizers/logistics excluded, fractional exactness"
```

---

## Task 6: Row filter predicate

**Files:**
- Modify: `ficsit-companion/src/domain/resource_flow.cpp`
- Modify: `ficsit-companion/tests/test_resource_flow.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_resource_flow.cpp`:
```cpp
/// @test   RowPassesFilter gates by status and case-insensitive name search:
///         All passes every status; Deficit/Surplus pass only matching rows;
///         empty search matches all; name search is case-insensitive.
/// @covers RowPassesFilter status gate + name search.
TEST_CASE("RowPassesFilter gates by status and name", "[resource_flow]")
{
    FlowFixture fx;
    ResourceFlowRow surplus;
    surplus.item = &fx.plate; // "Iron Plate"
    surplus.status = ResourceFlowStatus::Surplus;

    ResourceFlowRow deficit;
    deficit.item = &fx.ore;   // "Iron Ore"
    deficit.status = ResourceFlowStatus::Deficit;

    // Status filter
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, ""));
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::Surplus, ""));
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::Deficit, ""));
    REQUIRE(RowPassesFilter(deficit, ResourceFlowFilter::Deficit, ""));
    REQUIRE_FALSE(RowPassesFilter(deficit, ResourceFlowFilter::Surplus, ""));

    // Name search (case-insensitive), combined with All
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, "plate"));
    REQUIRE(RowPassesFilter(surplus, ResourceFlowFilter::All, "PLATE"));
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::All, "ore"));
    // Search still respects the status gate
    REQUIRE_FALSE(RowPassesFilter(surplus, ResourceFlowFilter::Deficit, "plate"));
}
```

- [ ] **Step 2: Run — expect FAIL**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: FAIL — the stub `RowPassesFilter` returns `true` for everything.

- [ ] **Step 3: Implement the predicate**

In `src/domain/resource_flow.cpp`, add to the anonymous namespace (near `Agg`):
```cpp
    bool ContainsCI(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) return true;
        if (needle.size() > haystack.size()) return false;
        const auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        {
            bool match = true;
            for (std::size_t j = 0; j < needle.size(); ++j)
            {
                if (lower(haystack[i + j]) != lower(needle[j])) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    }
```
Add `#include <cctype>` and `#include "domain/recipe.hpp"` (already included for `Item`) at the top. Replace the stub `RowPassesFilter` body with:
```cpp
bool RowPassesFilter(const ResourceFlowRow& row, ResourceFlowFilter filter, std::string_view search)
{
    if (filter == ResourceFlowFilter::Deficit && row.status != ResourceFlowStatus::Deficit) return false;
    if (filter == ResourceFlowFilter::Surplus && row.status != ResourceFlowStatus::Surplus) return false;
    if (row.item == nullptr) return search.empty();
    return ContainsCI(row.item->name, search);
}
```

- [ ] **Step 4: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[resource_flow]"
```
Expected: all `[resource_flow]` tests pass (8 cases).

- [ ] **Step 5: Commit**

```
git add ficsit-companion/src/domain/resource_flow.cpp ficsit-companion/tests/test_resource_flow.cpp
git commit -m "feat(resource-flow): row status+name filter predicate"
```

---

## Task 7: Persist the window-open setting

**Files:**
- Modify: `ficsit-companion/include/infra/settings_store.hpp`
- Modify: `ficsit-companion/src/infra/settings_store.cpp`
- Modify: `ficsit-companion/tests/test_settings_store.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_settings_store.cpp`, inside the existing `"SettingsStore round-trips scalar settings"` test, add a write before `store.Save(s);`:
```cpp
    s.show_resource_flow = true;
```
and a check after `store.Load(loaded, {});`:
```cpp
    REQUIRE(loaded.show_resource_flow == true);
```

- [ ] **Step 2: Run — expect FAIL (compile error)**

```
cmake --build build --config Release --target fc-tests
```
Expected: compile error — `Settings` has no member `show_resource_flow`.

- [ ] **Step 3: Add the field**

In `include/infra/settings_store.hpp`, inside `struct Settings` (after `left_panel_folded`):
```cpp
    /// @brief If true, the Resource Flow window is open.
    bool show_resource_flow = false;
```

- [ ] **Step 4: Load and save the key**

In `src/infra/settings_store.cpp`:
- In `Load`, after the `left_panel_folded` line:
  ```cpp
  out.show_resource_flow = json.contains("show_resource_flow") && json["show_resource_flow"].get<bool>(); // default false
  ```
- In `Save`, after the `left_panel_folded` line:
  ```cpp
  serialized["show_resource_flow"] = settings.show_resource_flow;
  ```

- [ ] **Step 5: Run — expect PASS**

```
cmake --build build --config Release --target fc-tests
.\build\ficsit-companion\Release\fc-tests.exe "[settings]"
```
Expected: `[settings]` tests pass.

- [ ] **Step 6: Commit**

```
git add ficsit-companion/include/infra/settings_store.hpp ficsit-companion/src/infra/settings_store.cpp ficsit-companion/tests/test_settings_store.cpp
git commit -m "feat(resource-flow): persist show_resource_flow setting"
```

---

## Task 8: Resource Flow window UI

No unit tests (consistent with the codebase — ImGui is not unit-tested). Verified by building the app and interacting.

**Files:**
- Modify: `ficsit-companion/include/app/production_app.hpp`
- Modify: `ficsit-companion/src/app/production_app.cpp`

- [ ] **Step 1: Declare state + method**

In `include/app/production_app.hpp`:
- Add the include near the other domain includes (after `#include "domain/graph_model.hpp"`):
  ```cpp
  #include "domain/resource_flow.hpp"
  ```
- Add to the `Render*` method group (after `void RenderLeftPanel();`):
  ```cpp
      /// @brief Render the toggleable Resource Flow window (per-item produced/
      /// consumed/net ledger). No-op when settings.show_resource_flow is false.
      void RenderResourceFlowWindow();
  ```
- Add state fields near the other render-state members (after `std::string recipe_filter;`):
  ```cpp
      ResourceFlowFilter resource_flow_filter = ResourceFlowFilter::All;
      std::string resource_flow_search;
  ```

- [ ] **Step 2: Add the left-panel toggle button**

In `src/app/production_app.cpp`, in `RenderLeftPanel()`, right after the `EndDisabled();` that follows the "Show controls list" button (the block ending at the line `ImGui::EndDisabled();` near the top of the function):
```cpp
    if (ImGui::Button(settings.show_resource_flow ? "Hide Resource Flow" : "Show Resource Flow"))
    {
        settings.show_resource_flow = !settings.show_resource_flow;
        settings_store->Save(settings);
    }
```

- [ ] **Step 3: Implement the window**

In `src/app/production_app.cpp`, add this method (place it directly after the `RenderLeftPanel()` definition):
```cpp
void ProductionApp::RenderResourceFlowWindow()
{
    if (!settings.show_resource_flow)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    bool open = settings.show_resource_flow;
    if (ImGui::Begin("Resource Flow", &open))
    {
        // Status filter buttons
        const auto filter_button = [this](const char* label, ResourceFlowFilter value) {
            const bool active = resource_flow_filter == value;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label)) resource_flow_filter = value;
            if (active) ImGui::PopStyleColor();
        };
        filter_button("All", ResourceFlowFilter::All);
        ImGui::SameLine();
        filter_button("Deficit", ResourceFlowFilter::Deficit);
        ImGui::SameLine();
        filter_button("Surplus", ResourceFlowFilter::Surplus);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", resource_flow_search.c_str());
        if (ImGui::InputTextWithHint("##resource_flow_search", "Search...", buf, sizeof(buf)))
        {
            resource_flow_search = buf;
        }

        // Non-const so the cached GetStringFloat() strings can be built per row.
        ResourceFlowReport report = BuildResourceFlowReport(nodes);

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##resource_flow_table", 5, flags))
        {
            ImGui::TableSetupColumn("Item");
            ImGui::TableSetupColumn("Produced/min");
            ImGui::TableSetupColumn("Consumed/min");
            ImGui::TableSetupColumn("Net/min");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            const float h = ImGui::GetTextLineHeightWithSpacing();
            for (ResourceFlowRow& row : report.rows)
            {
                if (!RowPassesFilter(row, resource_flow_filter, resource_flow_search))
                {
                    continue;
                }

                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                if (row.item != nullptr && row.item->icon_gl_index != 0)
                {
                    ImGui::Image((void*)(intptr_t)row.item->icon_gl_index, ImVec2(h, h));
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(row.item != nullptr ? row.item->name.c_str() : "(unknown)");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.produced.GetStringFloat().c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.consumed.GetStringFloat().c_str());

                ImGui::TableNextColumn();
                const ImVec4 green(0.4f, 0.85f, 0.4f, 1.0f);
                const ImVec4 red(0.9f, 0.4f, 0.4f, 1.0f);
                const std::string net_str =
                    (row.status == ResourceFlowStatus::Surplus ? "+" : "") +
                    row.net.GetStringFloat();
                if (row.status == ResourceFlowStatus::Surplus) ImGui::TextColored(green, "%s", net_str.c_str());
                else if (row.status == ResourceFlowStatus::Deficit) ImGui::TextColored(red, "%s", net_str.c_str());
                else ImGui::TextUnformatted(net_str.c_str());

                ImGui::TableNextColumn();
                switch (row.status)
                {
                case ResourceFlowStatus::Surplus:  ImGui::TextColored(green, "Surplus"); break;
                case ResourceFlowStatus::Deficit:  ImGui::TextColored(red, "Deficit"); break;
                case ResourceFlowStatus::Balanced: ImGui::TextUnformatted("Balanced"); break;
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::Text("%zu surplus  -  %zu deficit  -  %zu balanced",
            report.surplus_count, report.deficit_count, report.balanced_count);
        if (report.skipped_null_item_pins > 0)
        {
            ImGui::TextDisabled("%zu pin(s) with no item skipped", report.skipped_null_item_pins);
        }
    }
    ImGui::End();

    // The window's [x] close button toggles the persisted setting.
    if (open != settings.show_resource_flow)
    {
        settings.show_resource_flow = open;
        settings_store->Save(settings);
    }
}
```
Add `#include <cstdio>` near the top of `production_app.cpp` if not already present (for `std::snprintf`).

- [ ] **Step 4: Call the window each frame**

In `src/app/production_app.cpp`, in `RenderImpl()`, after the graph block's `ax::NodeEditor::End();` / `PopStyleColor()` lines (around line 594-597), add:
```cpp
    RenderResourceFlowWindow();
```

- [ ] **Step 5: Build the app**

```
cmake --build build --config Release --target ficsit-companion
```
Expected: builds with no errors.

- [ ] **Step 6: Manual smoke test**

Run the app, click **"Show Resource Flow"** in the left panel. Verify:
- A floating "Resource Flow" window appears with All/Deficit/Surplus buttons, a search box, the 5-column table, and a footer count line.
- Placing a craft node populates rows; Net is green for surplus, red for deficit.
- Filter buttons and search narrow the visible rows; the footer counts stay constant.
- Closing the window via its [x] keeps it closed after restarting the app (persisted).

- [ ] **Step 7: Commit**

```
git add ficsit-companion/include/app/production_app.hpp ficsit-companion/src/app/production_app.cpp
git commit -m "feat(resource-flow): toggleable resource flow window"
```

---

## Task 9: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Build everything**

```
cmake --build build --config Release
```
Expected: `fc-core`, `ficsit-companion`, and `fc-tests` all build.

- [ ] **Step 2: Run the full test suite**

```
ctest --test-dir build -C Release --output-on-failure
```
Expected: all tests pass, including the new `[resource_flow]` cases and the updated `[settings]` case.

- [ ] **Step 3: Commit any leftover (e.g. CMake comment)**

Optional: update the comment above `TEST_SOURCE_FILES` if desired. Then:
```
git add -A
git commit -m "chore(resource-flow): final verification"
```

---

## Self-review notes

- **Spec coverage:** calculation rules → Tasks 1–5; filter/search → Task 6; floating-window UI with filters/search/table/footer/icons → Task 8; window-state persistence → Task 7; all spec test scenarios → Tasks 1–6. Row sort (by name) asserted in Task 1.
- **Types are consistent** across tasks: `ResourceFlowStatus`, `ResourceFlowRow`, `ResourceFlowReport`, `ResourceFlowFilter`, `BuildResourceFlowReport`, `RowPassesFilter` are defined once (Task 1) and used unchanged thereafter.
- **`Item` access:** `name` and `icon_gl_index` come from `domain/recipe.hpp` (which defines `struct Item`); `resource_flow.cpp` includes it.
- **`FractionalNumber::GetStringFloat()` is non-const**, so Task 8 keeps the per-frame `report` (and its row references) non-const rather than const-casting.
