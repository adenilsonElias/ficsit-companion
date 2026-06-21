# Factory Snapshot — Zoom-Readable Nodes (Design)

Date: 2026-06-18
Status: Approved. Enhancement to the read-only snapshot graph
(`docs/superpowers/specs/2026-06-17-factory-snapshot-graph-view-design.md`,
`docs/superpowers/specs/2026-06-17-snapshot-node-icons-design.md`).

## Problem / goal

When the snapshot graph is zoomed out, every node looks the same — plain text
boxes — so you cannot tell a machine (production) from a merger/splitter, from
storage, from a station. Only the production node grows an icon (clamped to 3×),
which still becomes unreadable far out. The user wants, at a glance and at any
zoom:

1. Easy visual separation of the node categories: production, splitters/mergers,
   storage, stations (and sink).
2. Small recognizable "figures" (item icons) of what each node handles, visible
   when zoomed out.

## Categories

| Category    | Node kinds |
|-------------|------------|
| Production  | `Craft`, `Extractor` |
| Flow        | `CustomSplitter`, `GameSplitter`, `Merger`, Logistics `PipeJunction` |
| Storage     | Logistics `Storage`, `IndustrialStorage`, `DimensionalDepot`, `FluidBuffer`, `IndustrialFluidBuffer` |
| Station     | Logistics `TruckStation`, `TrainStation` |
| Sink        | `Sink` |
| Other       | `Group` (and any unmapped kind) |

## Design

### 1. Category classification (pure, testable)

Add to `domain/node_display.{hpp,cpp}`, beside `NodePrimaryItem`:

```cpp
enum class SnapshotCategory { Production, Flow, Storage, Station, Sink, Other };
SnapshotCategory NodeSnapshotCategory(const Node& node);
```

Classifies by `node.GetKind()`, and for `Kind::Logistics` further by
`static_cast<const LogisticsNode&>(node).logistics_kind` per the table above.
Pure — no GL, no `Data` — so it is unit-tested directly.

### 2. Category colors (always on, every zoom)

In `factory_snapshot_app.cpp`, a small lookup `CategoryColors(SnapshotCategory)`
returning `{ bg, border }` as `ImVec4` (what node-editor `PushStyleColor`
takes), medium saturation / slightly translucent:

| Category   | Background (approx)        | Border |
|------------|----------------------------|--------|
| Production | blue  `(60,110,200,200)`   | brighter blue |
| Flow       | orange`(200,130,40,200)`   | brighter orange |
| Storage    | green `(55,160,90,200)`    | brighter green |
| Station    | purple`(150,90,200,200)`   | brighter purple |
| Sink       | red   `(200,70,70,200)`    | brighter red |
| Other      | gray  `(110,110,120,200)`  | brighter gray |

(Exact RGBA tuned during the manual smoke test; the table above is the starting
point.) Applied per node by pushing the node-editor style colors around the
node:

```cpp
ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBg, bg);     // ImVec4
ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeBorder, border);
ax::NodeEditor::BeginNode(node.id);
...
ax::NodeEditor::EndNode();
ax::NodeEditor::PopStyleColor(2);
```

Because these are the node's real fill/border, the colored blocks remain
distinguishable even when nodes shrink to specks when zoomed out.
`StyleColor_NodeBg`, `StyleColor_NodeBorder`, `PushStyleColor(StyleColor, const
ImVec4&)` and `GetCurrentZoom()` are confirmed present in the vendored
imgui-node-editor header.

### 3. Level-of-detail in `RenderSnapshotNode`

Read `const float zoom = ax::NodeEditor::GetCurrentZoom();` (zoom < 1 == zoomed
out, consistent with the existing `1/zoom` icon scaling). Constants:

```cpp
constexpr float kCollapseZoom  = 0.5f;  // collapse when zoomed out past ~2x
constexpr float kMaxIconScale  = 5.0f;  // cap node-space icon growth
```

- **Collapsed** (`zoom < kCollapseZoom`): hide the title and all pin/rate
  *text*. Body is the node's primary item icon (`NodePrimaryItem`) drawn at
  `lineHeight * clamp(1.0f/zoom, 1.0f, kMaxIconScale)` so it stays roughly a
  constant size on screen as you zoom out. Pins are **still submitted** —
  `BeginPin` → 1px `ImGui::Dummy` → `EndPin`, input pins stacked in a group to
  the left of the icon, output pins to the right — so links keep attaching to
  valid pin rects; they simply lose their labels.
  - **No-item fallback**: when `NodePrimaryItem(node) == nullptr` (or its
    `icon_gl_index == 0`), draw a single-letter category glyph
    (`P`/`F`/`S`/`T`/`K`/`-`) on the colored tile instead of an icon.

- **Detailed** (`zoom >= kCollapseZoom`): unchanged from today — header product
  icon + title, per-pin icons + `item rate/min` text, vehicle-station plug.

The vehicle-station plug pin is also submitted (as a 1px dummy) in collapsed mode
so route (plug↔plug) links stay valid.

## Testing

- **Unit** (`tests/test_node_display.cpp`, no GL/Data): `NodeSnapshotCategory`
  returns Production for a `CraftNode`, Flow for a `MergerNode`, Storage for a
  `LogisticsNode{Storage}`, Station for a `VehicleStationNode{TrainStation}`,
  Sink for a `SinkNode`, Other for a `GroupNode`.
- **Manual smoke test** (GUI): load a save; zoom out — confirm categories are
  distinguishable by color and item icons stay readable; zoom in — confirm full
  detail (titles, per-pin icons, rates) returns and links render normally.

## Notes / non-goals

- No link coloring/styling changes.
- No balancing / read-only guarantees changed.
- No new files beyond extending `node_display.{hpp,cpp}` and
  `tests/test_node_display.cpp`; colors + LOD live in `factory_snapshot_app.cpp`.
- No git commits (git-preference); progress tracked in a PROGRESS.md.
- Exact color RGBA and the `kCollapseZoom` / `kMaxIconScale` thresholds are
  tuning values, finalized during the manual smoke test.
