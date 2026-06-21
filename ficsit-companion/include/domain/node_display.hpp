#pragma once

#include <memory>
#include <string>
#include <vector>

struct Node;
struct Item;

/// @brief Human-readable title for a node, by kind: recipe / extractor building
/// (+resource) / "Merger" / "Splitter" / "AWESOME Sink" / logistics display name /
/// group name. Pure: no UI, no editor context. Used by the read-only snapshot graph.
std::string NodeDisplayName(const Node& node);

/// @brief The node's representative ("product") item: first non-null output pin
/// item, else first non-null input pin item, else nullptr. Used to choose the big
/// per-node icon in the read-only snapshot graph. Pure: no UI.
const Item* NodePrimaryItem(const Node& node);

/// @brief Coarse visual category of a node, used by the read-only snapshot graph
/// to color nodes and pick a fallback glyph so categories stay distinguishable at
/// any zoom. Pure: no UI, no editor context.
enum class SnapshotCategory { Production, Flow, Storage, Station, Sink, Other };

/// @brief Classify a node into a SnapshotCategory by its kind (and, for logistics
/// nodes, by their logistics_kind). Pure.
SnapshotCategory NodeSnapshotCategory(const Node& node);

/// @brief Nodes that PRODUCE the named item: Craft or Extractor nodes with the
/// item on an output pin, in node order. Pass-through nodes (mergers/splitters/
/// storages) that merely carry it are excluded. Used to jump the snapshot graph
/// to a producer when a resource is clicked. Pure: no UI, no editor context.
std::vector<const Node*> NodesProducingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name);
