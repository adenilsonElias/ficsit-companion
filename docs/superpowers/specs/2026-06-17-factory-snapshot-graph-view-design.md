# Factory Snapshot — Read-only Node Graph View (Design)

Date: 2026-06-17
Status: Approved (design); ready for implementation plan.
Related: `docs/superpowers/specs/2026-06-16-factory-snapshot-design.md` (the original
list-first snapshot), `docs/superpowers/plans/2026-06-16-factory-snapshot-PROGRESS.md`.

## Problem

The Factory Snapshot tab currently renders only a list-first view (topology summary,
warnings, resource-flow table). The user wants it to look like the modeler (Production
Planner): the imported factory drawn as a node graph — but **read-only** (no editing) and
**without the balancer** (no rate propagation; rates shown exactly as imported).

## Goal

Render `FactorySnapshotModel`'s node/link graph as a modeler-style node graph in the
Factory Snapshot tab, read-only, while keeping topology/warnings/resource-flow accessible.

## Non-goals

- No graph editing of any kind (no add/delete nodes or links, no recipe/rate/clock edits).
- No rate balancing / propagation (`UpdateNodesRate` is never called).
- No reuse-by-extraction of the modeler's `ProductionApp::RenderNodes` (left untouched).
- No persistence of view state (pan/zoom/node drags are transient).

## Approach (chosen)

**Read-only node-editor graph with our own compact node drawing.** The Factory Snapshot
hosts its own `imgui-node-editor` context (so pan / zoom / link routing look and feel
exactly like the modeler) and draws each node itself as a compact read-only box. This
matches the modeler's feel with modest code and leaves the modeler's hottest code path
(`ProductionApp::RenderNodes`, ~hundreds of lines, tightly coupled) untouched.

Approaches rejected:
- *Reuse the modeler's exact node visuals* — would require extracting/sharing
  `RenderNodes`/`RenderLinks` out of `ProductionApp`: a large, risky refactor of core code.
- *Lightweight custom canvas* (like the Vehicle Map) — simplest, but would not match the
  modeler's look and feel.

## Architecture

- `FactorySnapshotApp` gains its **own** editor context:
  - `ax::NodeEditor::Config config;`
  - `ax::NodeEditor::EditorContext* context;`
  - Created in the constructor with `config.SettingsFile = nullptr` (no on-disk layout
    file), destroyed in the destructor via `ax::NodeEditor::DestroyEditor(context)`.
  - Independent of the modeler's context; each tool scopes its own `SetCurrentEditor` /
    `Begin` / `End`, so the two coexist without interference.
- The snapshot already owns `model.nodes` (`std::vector<std::unique_ptr<Node>>`) and
  `model.links` (`std::vector<std::unique_ptr<Link>>`), built by the shared importer
  (`SavImport::BuildGraph`) with node `pos` and per-pin `current_rate` already populated.
  Rendering reads these directly — **no new data structures**.
- Structurally read-only: the snapshot **never** calls `ax::NodeEditor::BeginCreate` /
  `BeginDelete`, and wires no add/drag-link/delete handlers. Node dragging, panning, and
  zooming remain available as view conveniences and are never written back to `node->pos`
  or to disk. The balancer is never invoked; pins display `current_rate` as imported.

## Components / methods (in `factory_snapshot_app.{hpp,cpp}`)

- `void RenderGraphCanvas();`
  - `ax::NodeEditor::SetCurrentEditor(context);`
  - If `needs_layout_apply`: for each node call `ax::NodeEditor::SetNodePosition(node->id, node->pos)`, then request a fit (`ax::NodeEditor::NavigateToContent()`), then clear the flag. (Applied here, inside the editor context, not in `LoadFromWrapperJson`.)
  - `ax::NodeEditor::Begin("##snapshot_graph");`
  - For each node: `RenderSnapshotNode(*node);`
  - `RenderSnapshotLinks();`
  - `ax::NodeEditor::End();`
  - `ax::NodeEditor::SetCurrentEditor(nullptr);`
- `void RenderSnapshotNode(const Node& node);`
  - `ax::NodeEditor::BeginNode(node.id);`
  - Title row: `NodeTitle(node)` text.
  - Two columns: inputs (`node.ins`) on the left, outputs (`node.outs`) on the right.
    Each pin: `ax::NodeEditor::BeginPin(pin.id, pin.direction)` → a simple pin marker
    (a small filled circle drawn via `ImGui::GetWindowDrawList()`, or a `"o"` glyph — no
    item-icon texture lookup needed) + item name (`pin.item ? pin.item->name : "(none)"`)
    + rate (`pin.current_rate.GetStringFraction()`) + "/min" → `EndPin()`. Input pins draw
    marker-then-label; output pins draw label-then-marker so they read inward/outward.
  - `ax::NodeEditor::EndNode();`
- `void RenderSnapshotLinks();`
  - For each `link` in `model.links`: `ax::NodeEditor::Link(link->id, link->start->id, link->end->id);`
    (skip any link whose `start`/`end` is null, defensively.)
- `static const char* / std::string NodeTitle(const Node& node);` (anonymous namespace
  free helper) — switch on `node.GetKind()`:
  - `Craft` → recipe display name (via `CraftNode::recipe`).
  - `Extractor` → extractor building name + resource item name.
  - `Merger` → "Merger"; `CustomSplitter` → "Splitter"; `GameSplitter` → "Splitter".
  - `Sink` → "AWESOME Sink".
  - `Logistics` → `LogisticsNode::GetDisplayName()` (covers Vehicle stations too).
  - `Group` → group name (not expected: the snapshot stores flat imported nodes, not a
    wrapping group; handled for completeness).

## Layout (`RenderImpl`)

1. Status line (existing `RenderStatusLine`).
2. `ImGui::BeginChild("##snapshot_left", ImVec2(340, 0), true)`:
   - `RenderTopologySummary()` (existing).
   - `RenderWarningsPanel()` (existing).
   - `if (ImGui::CollapsingHeader("Resource flow")) RenderResourceFlowTable();` — the
     existing filter/search/table, with `ImGuiTableFlags_ScrollX` added so the 4 columns
     fit in the narrow panel.
   - `ImGui::EndChild();`
3. `ImGui::SameLine();`
4. `RenderGraphCanvas()` fills the remaining width/height.
5. Empty state: when `model.nodes` is empty, the canvas area shows
   "Load a save from the bar above to populate this view." (no editor Begin needed).

## State added

- `ax::NodeEditor::Config config;`
- `ax::NodeEditor::EditorContext* context = nullptr;`
- `bool needs_layout_apply = false;` — set `true` at the end of `LoadFromWrapperJson`
  (after a successful build); consumed on the next `RenderGraphCanvas` frame to apply node
  positions and fit the view. This defers editor-context calls to render time (the same
  pattern the modeler uses for its deferred import).

## Data flow

`AppHost::DistributeJson` → `FactorySnapshotApp::LoadFromWrapperJson(json, options)` →
`FactorySnapshot::BuildSnapshotFromJson` populates `model` (nodes/links/flow/warnings) and
sets `needs_layout_apply = true`. Next frame, `RenderImpl` → `RenderGraphCanvas` applies
positions + fit and draws the graph. Pins display imported `current_rate`; no propagation.

## Error handling

- Build failure → existing behavior: `last_error` set, status line shows it, `model` empty,
  canvas shows the empty-state text.
- Defensive null checks: skip pins/links with null `item`/`start`/`end`; a node with no
  pins still renders its title box.

## Testing

- The canvas requires a live ImGui + node-editor context → not unit-testable headless.
- Add one unit test for the pure `NodeTitle()` helper (construct representative nodes,
  assert the label per kind) **if** it can be exercised without an editor context; if node
  construction pulls in too much, document it as manual-only and skip.
- Existing tests already cover the data path (`test_factory_snapshot_builder`,
  `test_factory_snapshot_session`, resource-flow tests). Keep them green (130/130).
- Manual GUI smoke test: load a `.sav`, open the Factory Snapshot tab, confirm the graph
  draws with titled nodes, per-pin item+rate labels, links, pan/zoom, and that the
  resource-flow collapsible still works.

## Risks / notes

- Two editor contexts (modeler + snapshot) live at once; each `Begin`/`End` is scoped by
  `SetCurrentEditor`, so they don't interfere.
- The importer already assigns valid `NodeId`/`PinId`s, required by the node editor.
- The left-panel resource-flow table is cramped at 340px → `ScrollX` keeps it usable.
- `node->pos` from the importer respects the shared layout option (Compact/World); the
  graph honors whatever layout the save was imported with.
