# Snapshot: cycle producers/consumers from the Produced/Consumed numbers

**Date:** 2026-06-29
**Status:** Approved (design)
**Area:** Factory Snapshot tool — resource-flow table + graph navigation

## Problem

In the Factory Snapshot tool, the resource-flow table (left panel, "Resources"
tab) lets the user click a row to jump the graph to a node that **produces** that
item; repeated clicks cycle through all producers. Today the whole row is a single
`Selectable` spanning all columns, so every click cycles producers.

The user wants the **Consumed** number to instead cycle through nodes that
**consume** the item, while the **Produced** number (and the item name) keep
cycling producers.

## Goal

- Clicking the **Produced** number cycles through producers of the item.
- Clicking the **Consumed** number cycles through consumers of the item.
- Clicking the **item name** keeps its current behavior (cycle producers).
- **Net** is not clickable.

## Non-goals

- No change to how rates are computed or displayed.
- No change to the graph rendering or the producer-jump mechanics beyond
  selecting which node set to cycle.
- Pass-through nodes (mergers/splitters/logistics/storage) are not navigation
  targets, consistent with the existing producer behavior.

## Design

### 1. New domain helper: `NodesConsumingItem`

File: `ficsit-companion/include/domain/nodes/node_display.hpp` (declaration),
`ficsit-companion/src/domain/nodes/node_display.cpp` (definition).

Mirror of the existing `NodesProducingItem`, but matching **input** pins on
consuming node kinds:

```cpp
/// @brief Nodes that CONSUME the named item: Craft or Sink nodes with the item
/// on an input pin, in node order. Pass-through nodes (mergers/splitters/
/// storages/logistics) that merely carry it are excluded. Used to jump the
/// snapshot graph to a consumer when a resource's Consumed value is clicked.
/// Pure: no UI, no editor context.
std::vector<const Node*> NodesConsumingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name);
```

Implementation: iterate `nodes`; skip null; keep a node when
`node->IsCraft() || node->IsSink()` and any pin in `node->ins` has
`pin->item->name == item_name`; push once per node (break after first match), in
node order.

### 2. Navigation state gains a mode

File: `ficsit-companion/include/app/factory_snapshot_app.hpp`.

Add a small enum and a mode field alongside the existing nav state:

```cpp
enum class NavMode { Produce, Consume };
```

- `NavMode nav_mode = NavMode::Produce;` — which set the pending request targets.
- `NavMode nav_cycle_mode = NavMode::Produce;` — cycle bookkeeping companion to
  `nav_cycle_item`.

The cycle index resets when **either** the item **or** the mode changes:
`nav_item != nav_cycle_item || nav_mode != nav_cycle_mode`. This makes switching
Produced↔Consumed on the same item restart the cycle at index 0.

### 3. Per-cell click targets in `RenderResourceFlowTable`

File: `ficsit-companion/src/app/factory_snapshot_app.cpp`.

Replace the single all-columns `Selectable` with per-cell targets. Push a unique
id scope per row (e.g. `ImGui::PushID(row_index)`) because the displayed cell
text can repeat across rows.

- **Item name** cell: `Selectable` (no longer `SpanAllColumns`) → on click set
  `nav_item = row.item->name; nav_mode = NavMode::Produce;`.
- **Produced** cell: `Selectable` showing `row.produced.GetStringFloat()` → on
  click set `nav_item = ...; nav_mode = NavMode::Produce;`.
- **Consumed** cell: `Selectable` showing `row.consumed.GetStringFloat()` → on
  click set `nav_item = ...; nav_mode = NavMode::Consume;`.
- **Net** cell: unchanged plain `TextUnformatted`.

### 4. Consume the request in `RenderGraphCanvas`

File: `ficsit-companion/src/app/factory_snapshot_app.cpp`.

When `nav_item` is non-empty, pick the target set by mode:

```cpp
const std::vector<const Node*> targets =
    nav_mode == NavMode::Produce
        ? NodesProducingItem(model.nodes, nav_item)
        : NodesConsumingItem(model.nodes, nav_item);
```

Reset the cycle on item-or-mode change, then select/navigate/advance exactly as
today, updating `nav_cycle_item` and `nav_cycle_mode` together.

## Edge cases

- **No targets** (e.g. clicking Consumed on a final product nobody consumes):
  empty list → no-op, identical to today's empty-producers behavior.
- **Mode switch on same item:** cycle restarts at index 0.
- **Repeated id collisions:** avoided by `PushID(row_index)` around each row's
  cells.

## Testing

- Catch2 unit test for `NodesConsumingItem` mirroring the existing
  `NodesProducingItem` test in `tests/test_node_display.cpp`: a `CraftNode` with
  the item on an input pin (and a `SinkNode` consuming it) is returned; a node
  that only outputs the item, and pass-through nodes, are not; unknown item
  returns empty.
- UI wiring (`RenderResourceFlowTable`/`RenderGraphCanvas`) is exercised
  manually, consistent with the rest of the snapshot UI which has no automated
  ImGui tests.
