# Factory Snapshot — Left-panel Tabs + Click-to-Producer (Design)

Date: 2026-06-18
Status: Approved. Builds on the read-only snapshot graph
(`2026-06-17-factory-snapshot-graph-view-design.md`).

## Problem / goal

The snapshot left panel crams topology + warnings + a collapsible resource-flow table, so the
table is cramped. Make the resource flow fill the left panel, move topology/warnings into a small
secondary tab, and let clicking a resource jump the graph to a machine producing it.

## Design

### Left panel → TabBar
Replace the topology + warnings + collapsible-flow stack in `RenderImpl`'s left child with a
`TabBar`:
- **Resources** tab (default): `RenderResourceFlowTable()`, with the table sized to the panel's
  remaining height (`outer_size.y = ImGui::GetContentRegionAvail().y` at the table) instead of the
  current fixed 12-row box, so it fills.
- **Info** tab: `RenderTopologySummary()` + separator + `RenderWarningsPanel()`.

### Click a resource → jump to a producer
- In `RenderResourceFlowTable`, the Item cell becomes an `ImGui::Selectable`. On click, set a
  pending `nav_item = row.item->name`.
- `RenderGraphCanvas` (runs inside the editor context) consumes the pending target each frame:
  gather the producers of `nav_item`, then `ax::NodeEditor::SelectNode(id)` +
  `NavigateToSelection()` on the current cycle entry, then clear `nav_item`. A per-item cycle
  index advances each click and resets when the clicked item changes — mirrors
  `ProductionApp::FocusNextItem`. (Deferred to the canvas because navigation requires the editor
  context, which the left panel is not inside.)
- **Producer definition:** a node that `IsCraft()` or `IsExtractor()` and has the item on an
  OUTPUT pin. Pass-through nodes (mergers/splitters/storages) are excluded.

### Pure helper (testable)
`std::vector<const Node*> NodesProducingItem(const std::vector<std::unique_ptr<Node>>& nodes,
const std::string& item_name)` added to `domain/node_display.{hpp,cpp}`: returns, in node order,
the Craft/Extractor nodes with `item_name` on an output pin. No UI, no editor context.

### State added (transient, not persisted)
- `std::string nav_item;` — pending click target consumed by the canvas.
- `std::string nav_cycle_item; int nav_cycle_index = 0;` — cycle bookkeeping.

## Testing

- Unit-test `NodesProducingItem` (no GL/Data): an `ExtractorNode(MinerMk1, &iron, ...)` is returned
  for "Iron Ore"; a `MergerNode` carrying the same item on its output is NOT returned (not a
  producer); an unknown item yields an empty list.
- Tab layout and navigation are GUI → manual smoke test (click a resource, graph centers on a
  producer; click again to cycle).

## Non-goals

- No persistence of the active tab or cycle index.
- No change to read-only / no-balancer behavior. Single file (`factory_snapshot_app.cpp`) plus the
  pure helper + its test. No git commits (git-preference).
