# Snapshot: Per-Item Production Filter

**Date:** 2026-06-29
**Tool affected:** Factory Snapshot (read-only `.sav` view)
**Status:** Design approved, pending spec review

## Goal

Add per-item visibility checkboxes to the Factory Snapshot's **Resources** tab.
Every produced item starts checked (visible). Unchecking an item hides the
machines/extractors that produce it from the graph canvas — a **pure hide**
(node and its links disappear; no rerouting). The underlying imported model is
never mutated; this is a visualization-only, toggleable view preference. A
**Select all / Unselect all** pair toggles every producible item at once.

Motivation: large imported saves have so many machines that isolating one
production line for debugging is hard. Hiding unrelated productions declutters
the canvas without deleting anything.

## Behavior summary

- **Per produced item:** one checkbox per item that is produced. Unchecking
  hides every Craft/Extractor node producing that item.
- **Pure hide (no reroute):** a hidden producer and all its links vanish from
  the canvas. Downstream consumers simply lose that incoming link; no synthetic
  producer→consumer edge is drawn. (Contrast with the existing logistics filter,
  which *bypasses* hidden nodes.)
- **Multi-output safe:** a machine with several outputs stays visible if *any*
  of its output items is still checked; it is hidden only when *every* item it
  produces is unchecked.
- **Visualization-only:** the Topology summary and the Produced/Consumed/Net
  numbers continue to reflect the full imported data. Only the graph canvas and
  the per-row checkbox state change.
- **Default = all visible:** persisted state stores the *unchecked* items, so an
  empty set means everything is visible, and newly imported items default to
  visible.

## Producer definition

Reuses the existing `NodesProducingItem` semantics: a node "produces" item X if
it `IsCraft()` or `IsExtractor()` and has an output pin whose item is X.
Splitters, mergers, logistics, and sinks are never producers and are unaffected
by this filter (they remain governed by the existing logistics filter).

## Architecture

Render-time filtering via pure domain helpers; the model
(`FactorySnapshotModel`) stays read-only. The app maintains a set of hidden item
names, derives a per-node hidden predicate, and recomputes the canvas edge list
when the set changes.

Rejected alternatives:
- **Mutate / delete nodes on uncheck** — destroys the read-only invariant and
  cannot be toggled back without re-import.
- **Hide + reroute (bypass) like logistics** — produces misleading direct edges
  for producers (a machine's inputs would connect straight to its output's
  consumers, mixing item identities). Explicitly rejected by the user.

## Component 1 — session persistence

Add to `FactorySnapshotSession` (mirrors the existing additive fields):

```cpp
/// @brief Item names whose producers (Craft/Extractor) are hidden on the
/// snapshot canvas. Empty by default => every production visible. Storing the
/// *hidden* set (not the visible one) keeps the default empty and makes newly
/// imported items visible automatically. Visualization-only.
std::vector<std::string> hidden_production_items;
```

- `Serialize()` writes a JSON array of strings.
- `Deserialize()` reads the array when present; a missing key leaves the vector
  empty (all visible). Non-string / malformed entries are ignored.

## Component 2 — production-hide predicate (domain)

New pure free function alongside `NodesProducingItem` in
`domain/nodes/node_display.{hpp,cpp}`:

```cpp
// True when `node` is a producer (Craft or Extractor) that has at least one
// output item AND every one of its output items is present in `hidden_items`.
// Non-producers and producers with any still-visible output item return false.
bool NodeProductionHidden(const Node& node,
                          const std::set<std::string>& hidden_items);
```

Pure and dependency-light → unit tested directly with Catch2.

## Component 3 — pure-hide vs bypass in `ComputeVisibleEdges`

The logistics filter bypasses (reroutes through) hidden nodes; production hides
must drop them. Extend `ComputeVisibleEdges` with a second, defaulted predicate
so existing callers and tests are unchanged:

```cpp
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden,
    const std::function<bool(const Node&)>& should_bypass
        = [](const Node&) { return true; });
```

Traversal change: during the forward walk, when the walk reaches a **hidden**
node, consult `should_bypass(node)`:
- `true`  → recurse through that node's output links (current behavior).
- `false` → emit nothing and stop (the node and its links are dropped).

`is_hidden` still controls which nodes are *not drawn*; `should_bypass`
distinguishes reroute-hide (logistics) from drop-hide (productions). The two
hidden sets never overlap (logistics kinds vs Craft/Extractor), so the default
preserves the existing logistics behavior exactly.

## Component 4 — app integration (`FactorySnapshotApp`)

- Maintain a `std::set<std::string>` cache of `hidden_production_items`, rebuilt
  whenever the set changes (fast membership lookup during rendering).
- `IsNodeHidden(node)`: return true if the logistics filter hides it **or**
  `NodeProductionHidden(node, hidden_set)` is true.
- `RebuildVisibleEdges()`: call `ComputeVisibleEdges` with
  `should_bypass = [logistics-hidden only]`, so production-hidden nodes drop
  while logistics-hidden nodes still reroute.

## Component 5 — Resources tab UI (`RenderResourceFlowTable`)

- Add a leading **"Show"** checkbox column; the table grows from 4 to 5 columns
  (Show, Item, Produced, Consumed, Net).
- Render an interactive checkbox only for rows where `row.produced != 0` (the
  item has producers to hide). Consumed-only rows render an empty cell.
- Checkbox checked == item NOT in `hidden_production_items`. Toggling adds or
  removes the item name, then calls `SaveSession()`, rebuilds the set cache, and
  calls `RebuildVisibleEdges()`.
- Above the table (near the filter radios / search box), add **Select all**
  (clear `hidden_production_items`) and **Unselect all** (insert every producible
  item name) buttons; each saves + rebuilds.
- The existing All/Deficit/Surplus radios and the search box still only affect
  which rows are *listed*, not which productions are hidden.

Note: the checkbox uses `row.produced != 0` to decide producibility, which is
consistent with the flow report for flat imported snapshots (no Group nodes).

## Testing

- `test_node_display.cpp` — `NodeProductionHidden`:
  - single-output producer whose item is hidden → true;
  - producer with one hidden + one visible output → false;
  - producer with all outputs hidden → true;
  - non-producer (splitter/merger/logistics/sink) → always false;
  - empty hidden set → false.
- `test_snapshot_bypass.cpp` — drop behavior:
  - a dropped node (`should_bypass` false) emits no synthetic edges; both its
    inbound and outbound links vanish;
  - a bypass node (`should_bypass` true) still reroutes (regression);
  - mixed chain visible → bypass-hidden → drop-hidden → consumer terminates at
    the dropped node (no edge to the consumer);
  - default overload (no `should_bypass`) preserves current bypass behavior.
- `test_factory_snapshot_session.cpp` — `hidden_production_items`:
  - round-trip of a populated list;
  - default empty when key missing (merge preserves other fields);
  - serialized output contains the array.

## Out of scope

- No changes to the Topology summary or the Produced/Consumed/Net values.
- No master on/off toggle — the empty hidden set is the "off" state and
  Select-all restores it.
- No changes to other tools (Production, Vehicle Map).
- No persistence format migration (additive key; older sessions default empty).
