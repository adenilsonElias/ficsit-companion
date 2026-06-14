# Resource Flow Tool — Design

**Date:** 2026-06-11
**Status:** Approved (pending spec review)

## Problem

Ficsit Companion lets users build a node graph of a Satisfactory factory, but
offers no single view answering "across everything I've placed, which resources
am I short on and which do I have to spare?" The left panel summarizes inputs and
outputs, but there is no per-item gross produced-vs-consumed ledger that marks
each resource as deficit, surplus, or balanced.

This design adds a **Resource Flow** tool: a pure, reusable calculation that
scans the in-memory graph and produces a per-item report, plus a toggleable
floating window that renders it as a filterable table.

This supersedes and refines the earlier `docs/resource-flow-implementation-plan.md`,
extending it from "calculation only" to "calculation core + UI panel" while
keeping the calculation logic pure and independently testable.

## Goal

1. A pure domain module that turns the node graph into a `ResourceFlowReport`.
2. Pure filter/search helpers for the table.
3. A toggleable floating ImGui window rendering the report.
4. A Catch2 test file covering the calculation and helper logic.

Non-goals: editing the graph from the panel, exporting the report, planning how
to resolve a deficit, or persisting the report itself.

## Architecture

Follows the pattern already used by the Vehicle Map tool (`VehicleMapQuery` pure
logic + app-layer rendering): all calculation lives in `fc-core`-linked domain
modules with Catch2 tests; `ProductionApp` only renders.

```
domain/resource_flow.{hpp,cpp}   ← pure: report + filter/search (tested)
        │  BuildResourceFlowReport(nodes) -> ResourceFlowReport
        ▼
app/production_app.cpp           ← RenderResourceFlowWindow() (not unit-tested)
```

The report depends only on `std::vector<std::unique_ptr<Node>>`. Links are
irrelevant: gross produced/consumed totals are independent of how pins are wired,
which is also why a fully internal intermediate (e.g. Iron Plate produced and
consumed 600/600) naturally reports as `Balanced`.

## Module: `domain/resource_flow.{hpp,cpp}`

### Types

```cpp
enum class ResourceFlowStatus { Deficit, Balanced, Surplus };

struct ResourceFlowRow
{
    const Item* item;
    FractionalNumber produced;
    FractionalNumber consumed;
    FractionalNumber net;        // produced - consumed
    ResourceFlowStatus status;
};

struct ResourceFlowReport
{
    std::vector<ResourceFlowRow> rows;   // sorted ascending by item->name
    size_t skipped_null_item_pins = 0;
    size_t surplus_count = 0;
    size_t deficit_count = 0;
    size_t balanced_count = 0;
};

ResourceFlowReport BuildResourceFlowReport(
    const std::vector<std::unique_ptr<Node>>& nodes);
```

### Calculation rules

Walk every node; for each, read `Pin::current_rate` (exact `FractionalNumber`,
the live propagated rate):

| Node kind (predicate) | Contribution |
| --- | --- |
| `IsCraft()` | `ins` → consumed, `outs` → produced |
| `IsExtractor()` | `outs` → produced |
| `IsSink()` | `ins` → consumed |
| `IsGroup()` | recurse into `group->nodes` (gross internal visibility) |
| `IsOrganizer()` (Merger / CustomSplitter / GameSplitter) | none |
| `IsLogistics()` (Storage / IndustrialStorage / Truck / Train / DimensionalDepot) | none |

- Accumulate into a `std::map<const Item*, {produced, consumed}, ItemPtrCompare>`
  for deterministic iteration.
- Any pin with `item == nullptr` increments `skipped_null_item_pins` and is not
  tallied.
- Per item: `net = produced − consumed`. `net > 0` → Surplus, `net < 0` →
  Deficit, `net == 0` → Balanced.
- Emit one `ResourceFlowRow` per item, **sorted ascending by `item->name`**.
- Tally `surplus_count` / `deficit_count` / `balanced_count` over all rows.

Group recursion is depth-first and unconditional, so nested groups are fully
included. A group's own cached `inputs`/`outputs` maps are deliberately NOT used
(they are net, not gross, and group-only).

### Filter/search helpers (pure)

```cpp
enum class ResourceFlowFilter { All, Deficit, Surplus };

// Case-insensitive substring match on item->name; empty search matches all.
bool RowPassesFilter(const ResourceFlowRow& row,
                     ResourceFlowFilter filter,
                     std::string_view search);
```

`filter` narrows by status (`All` passes every status; `Deficit`/`Surplus` pass
only that status — balanced rows are hidden under either non-`All` filter).
`search` narrows by item name. Footer counts always come from the full report,
independent of the active filter/search.

## UI: `ProductionApp::RenderResourceFlowWindow()`

New private render method plus state on `ProductionApp`:

- `bool show_resource_flow` — window open flag, **persisted in `settings.json`**
  alongside the existing toggles (`show_somersloop`, `left_panel_folded`, …).
- `ResourceFlowFilter resource_flow_filter` — defaults to `All`.
- `std::string resource_flow_search` — search input (via `InputText`, same as the
  existing `recipe_filter`).

Behavior:

- A **"Resource Flow" button in `RenderLeftPanel()`** toggles `show_resource_flow`.
- When open, `RenderResourceFlowWindow()` calls `BuildResourceFlowReport(nodes)`
  once per frame (graphs are small — tens to low-hundreds of nodes) and draws an
  `ImGui::Begin` window containing:
  - Filter buttons `[All] [Deficit] [Surplus]` (highlight the active one).
  - A search `InputText`.
  - A 5-column `ImGui::BeginTable`: **Item** (icon via
    `ImGui::Image((void*)(intptr_t)item->icon_gl_index, …)` + name),
    **Produced/min**, **Consumed/min**, **Net/min** (green when positive, red when
    negative), **Status** (Deficit / Surplus / Balanced). Rows filtered via
    `RowPassesFilter`.
  - A footer line: `N surplus · N deficit · N balanced` from the report counts.
  - If `skipped_null_item_pins > 0`, a small muted note showing the skipped count.
- Rates are rendered with the same `FractionalNumber` display formatting already
  used by the left-panel rate display; the `Net/min` cell is prefixed with `+`
  when positive.

The window is purely a viewer: it never mutates the graph.

## Testing — `tests/test_resource_flow.cpp`

Catch2, fabricating nodes the same way `test_rate_solver.cpp` /
`test_group_node.cpp` do (dummy `Building`/`Item`/`Recipe`, `IdGen`,
`graph_test_helpers.hpp`). Each `TEST_CASE` carries a `/// @test` + `/// @covers`
docstring, consistent with the rest of the suite.

Calculation scenarios:

1. Single craft node → its inputs counted as consumed, outputs as produced.
2. Craft chain → intermediate item shows gross produced and gross consumed
   (balanced when fully internal).
3. Extractor feeding a craft → raw-resource deficit reduced/removed.
4. Sink input → appears as consumption (deficit for that item).
5. Organizers (merger/splitter) and logistics (storage/truck/train/depot) →
   contribute no production or consumption.
6. Group and nested group contents → included recursively (gross).
7. Fractional rates → exact deficit/surplus values preserved (e.g. 9/2).
8. Null-item pins → not tallied; `skipped_null_item_pins` counts them.
9. Report counts → `surplus_count`/`deficit_count`/`balanced_count` and row sort
   order are correct.

Helper scenarios:

10. `RowPassesFilter` — `All` passes all statuses; `Deficit`/`Surplus` pass only
    matching statuses; case-insensitive name search; empty search matches all.

The ImGui window is not unit-tested (consistent with the codebase — no ImGui
tests exist).

## Wiring

- Add `domain/resource_flow.hpp` / `domain/resource_flow.cpp` to the `fc-core`
  source list in `ficsit-companion/CMakeLists.txt`.
- Add `tests/test_resource_flow.cpp` to the `fc-tests` source list.
- Include `domain/resource_flow.hpp` in `production_app.cpp`; add the
  `show_resource_flow` field to the persisted `Settings` and to
  `SettingsStore` load/save.

## Decisions (resolved during brainstorming)

- **Scope:** calculation core + floating-window UI (not core-only).
- **Placement:** toggleable floating window, matching the approved mockup
  (`docs/resource-flow-layout.png`).
- **Row order:** sorted ascending by item name (deterministic for tests and
  scanning).
- **Window state:** open/closed flag persisted in `settings.json`.

## Assumptions

- The approved mockup remains at `docs/resource-flow-layout.png`.
- Reusable calculation logic is built and tested before UI integration.
