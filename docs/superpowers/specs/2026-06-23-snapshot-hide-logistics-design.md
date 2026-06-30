# Snapshot: Hide & Bypass Splitters / Mergers / Logistics

**Date:** 2026-06-23
**Tool affected:** Factory Snapshot (read-only `.sav` view)
**Status:** Design approved, pending spec review

## Goal

Add an option to the Factory Snapshot graph view that removes splitter, merger,
and logistics nodes from the canvas for easier visualization, re-routing the
flow so producers connect directly to consumers (transitive bypass). The
underlying imported model is never mutated — this is a visualization-only,
toggleable view preference.

## Behavior summary

- **Bypass / re-route (not just hide):** when a node is hidden, its upstream
  producer(s) are reconnected directly to its downstream consumer(s).
- **Transitive / chain-collapsing:** a chain such as splitter → merger →
  splitter collapses into direct producer→consumer edges.
- **Endpoint-safe:** a hidden node with no visible downstream (e.g. a truck/train
  station that is a route endpoint in this graph) simply disappears with its
  links — no misleading direct edge is drawn.
- **Visualization-only:** the Topology summary counts and the Resource-flow
  table continue to show the full imported data. Only the graph canvas changes.

## Node kinds in scope

Configurable per-kind: `Node::Kind::GameSplitter`, `Node::Kind::CustomSplitter`,
`Node::Kind::Merger`, `Node::Kind::Logistics`.

## Architecture

Approach: render-time bypass via a pure domain function. The model
(`FactorySnapshotModel`) stays untouched and read-only; its resource-flow and
topology data remain correct. The app computes a set of *effective edges* for
the canvas and skips hidden nodes when rendering.

Rejected alternatives:
- **Mutate the model on toggle** — destroys the read-only invariant, breaks the
  resource-flow report, cannot be toggled back without re-import.
- **Hide-only (no reroute)** — leaves downstream machines visually dangling.

## Component 1 — domain bypass function

New files: `domain/snapshot/snapshot_bypass.hpp` / `.cpp`.

```cpp
struct SnapshotEdge {
    ax::NodeEditor::PinId start_id; // an output pin on a visible node
    ax::NodeEditor::PinId end_id;   // an input pin on a visible node
};

// For every output pin of a visible node, walk forward along its link:
//  - if the link's end pin belongs to a VISIBLE node, emit that edge directly;
//  - if it belongs to a HIDDEN node, recurse through that node's output links
//    (DFS, visited-guarded against cycles) until reaching visible input pins,
//    emitting one synthetic edge per reached visible input pin.
// Hidden endpoints with no visible downstream produce no edge.
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden);
```

Traversal relies on existing graph wiring: `Link::start` / `Link::end` are
`Pin*`; each `Pin` knows its `node` and its single `link`. Forward walk from a
visible output pin follows `link->end->node`; if hidden, iterate that node's
`outs`, follow each `out->link->end`, and recurse.

Cycle guard: a `visited` set of hidden node ids prevents infinite loops on
malformed cyclic data.

This function is pure and dependency-light → covered directly with Catch2.

## Component 2 — session persistence

Add to `FactorySnapshotSession` (mirrors the existing slider fields: JSON
round-trip, per-key merge on deserialize, defaults preserved):

```cpp
bool hide_logistics_enabled = false;   // master toggle (off by default)
bool hide_game_splitters   = true;     // per-kind selections
bool hide_custom_splitters = true;
bool hide_mergers          = true;
bool hide_logistics_nodes  = true;
```

`Serialize()` writes all five; `Deserialize()` merges each key when present,
leaving defaults otherwise.

## Component 3 — Options panel UI

In `RenderOptionsPanel`, below the existing sliders:

- Master `Checkbox("Hide splitters / mergers / logistics", &hide_logistics_enabled)`.
- Four indented per-kind checkboxes (Game Splitters, Custom Splitters, Mergers,
  Logistics), wrapped in `ImGui::BeginDisabled(!hide_logistics_enabled)` /
  `EndDisabled`.
- Any change calls `SaveSession()` and invalidates the cached edge list.
- "Reset to defaults" also resets the five new fields to their defaults.

## Component 4 — canvas integration

App builds the hidden-node predicate from the session:

```cpp
auto is_hidden = [&](const Node& n) -> bool {
    if (!session.hide_logistics_enabled) return false;
    switch (n.GetKind()) {
        case Node::Kind::GameSplitter:   return session.hide_game_splitters;
        case Node::Kind::CustomSplitter: return session.hide_custom_splitters;
        case Node::Kind::Merger:         return session.hide_mergers;
        case Node::Kind::Logistics:      return session.hide_logistics_nodes;
        default:                         return false;
    }
};
```

`RenderGraphCanvas`:
- Node loop: `if (is_hidden(*node)) continue;` before `RenderSnapshotNode`.
- Replace `RenderSnapshotLinks()` with drawing the cached effective-edge list.

**Edge cache & link ids:** compute `ComputeVisibleEdges` once and cache the
result, assigning one stable editor link id per edge from `NextId()`. Recompute
when (a) a new import loads (same trigger as `needs_layout_apply`) or (b) any of
the five toggle fields change. This keeps link ids stable across frames and
avoids per-frame traversal cost. When the master toggle is off, the cache equals
the original links (every node visible), preserving current behavior.

## Testing

- `test_snapshot_bypass.cpp` (Catch2):
  - direct visible→visible edge passes through unchanged;
  - single splitter bypassed → producer connects to each downstream consumer;
  - single merger bypassed → each upstream producer connects to the consumer;
  - chain splitter→merger→splitter collapses to direct edges;
  - hidden endpoint (no visible downstream) produces no edge;
  - cyclic hidden nodes terminate (visited guard);
  - predicate selecting only a subset of kinds leaves other kinds in place.
- Extend `test_factory_snapshot_session.cpp` to cover serialize/deserialize of
  the five new fields (including default-preservation on missing keys).

## Out of scope

- No changes to Topology summary or Resource-flow table.
- No change to other tools (Production, Vehicle Map).
- No persistence format migration concerns (additive keys, older sessions just
  use defaults).
