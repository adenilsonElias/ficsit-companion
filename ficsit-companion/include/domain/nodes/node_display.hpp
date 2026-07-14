#pragma once

#include <memory>
#include <set>
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

/// @brief Nodes that CONSUME the named item: Craft or Sink nodes with the item
/// on an input pin, in node order. Pass-through nodes (mergers/splitters/
/// storages/logistics) that merely carry it are excluded. Used to jump the
/// snapshot graph to a consumer when a resource's Consumed value is clicked.
/// Pure: no UI, no editor context.
std::vector<const Node*> NodesConsumingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name);

/// @brief True when `node` is a producer (Craft or Extractor) that touches at
/// least one item AND every item on ALL its pins — consumed inputs and produced
/// outputs alike — is present in `hidden_items`. A machine that still consumes or
/// produces any visible item stays visible, so "show only Cable" keeps the
/// machine that consumes Cable to make a hidden product. Non-producers return
/// false. Used to pure-hide a production from the read-only snapshot canvas. Pure.
bool NodeProductionHidden(const Node& node, const std::set<std::string>& hidden_items);

/// @brief True when `node` is a pass-through logistics/flow node (splitter,
/// merger, or logistics station/storage) that carries at least one item AND every
/// item on its pins is present in `hidden_items`. Producers, sinks, and logistics
/// nodes with any still-visible pin item (or no resolved item) return false.
/// Lets hiding an item also hide the splitters/mergers/stations/storages that
/// carry only that item; such nodes reroute (bypass) rather than drop. Pure: no UI.
bool NodeLogisticsHiddenByItems(const Node& node, const std::set<std::string>& hidden_items);
