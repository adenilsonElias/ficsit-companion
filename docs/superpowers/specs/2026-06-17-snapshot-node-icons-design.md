# Factory Snapshot — Item Icons on Nodes (Design)

Date: 2026-06-17
Status: Approved. Small enhancement to the read-only snapshot graph
(`docs/superpowers/specs/2026-06-17-factory-snapshot-graph-view-design.md`).

## Problem / goal

The snapshot graph labels nodes/pins with text only. The user wants item icons on the
nodes so productions are recognizable when zoomed out.

## Design

- **Icon source:** every `Item` already has `icon_gl_index` (a GL texture loaded at
  startup). Draw with `ImGui::Image((void*)(intptr_t)item->icon_gl_index, size)` — the same
  call the modeler uses. No new loading.
- **Primary product item:** new pure helper `const Item* NodePrimaryItem(const Node&)` added
  next to `NodeDisplayName` in `domain/node_display.{hpp,cpp}`: returns the first non-null
  **output** pin item, else the first non-null **input** pin item, else `nullptr`. Covers
  craft→output, extractor→resource, sink→input, merger/splitter→their item.
- **Header product icon (zoom-out recognition):** in `RenderSnapshotNode`, draw the primary
  item's icon before the title, sized `lineHeight * clamp(1/GetCurrentZoom(), 1, 3)`. As the
  view zooms out, the node-space icon grows so it stays a roughly constant, recognizable
  size on screen (same zoom-scaling approach the modeler uses).
- **Per-pin icons:** replace the plain circle marker in each pin with that pin's item icon
  (at `lineHeight`), kept next to the `item rate/min` text — input = icon then label, output
  = label then icon. Fall back to the existing circle marker when a pin has no item or no
  texture (`icon_gl_index == 0`). The vehicle-station plug keeps the circle marker.

## Testing

- Unit-test `NodePrimaryItem` (pure, no GL/Data): a `CustomSplitterNode` built with a local
  dummy `Item` returns it (output path); a `SinkNode` with a dummy item returns it (input
  path); a `SinkNode` with no item returns `nullptr`.
- Icon drawing is GUI → manual smoke test (zoom out, confirm product icons stay readable;
  zoom in, confirm per-pin icons).

## Notes / non-goals

- No content level-of-detail beyond the icon zoom-scaling (text/pins are not hidden when
  zoomed out). Can be added later if needed.
- Read-only / no-balancer guarantees unchanged. No new files beyond extending
  `node_display.{hpp,cpp}` and its test. No git commits (git-preference).
