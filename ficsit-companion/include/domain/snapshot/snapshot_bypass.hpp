#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <imgui_node_editor.h>

struct Node;
struct Link;

/// @brief An effective canvas edge after hidden nodes are bypassed. Both pins
/// belong to *visible* nodes; the edge may correspond to a real link or to a
/// path routed transitively through one or more hidden nodes.
struct SnapshotEdge
{
    ax::NodeEditor::PinId start_id; // output pin on a visible node
    ax::NodeEditor::PinId end_id;   // input pin on a visible node
};

/// @brief Compute the edges to draw when some nodes are hidden. For every link
/// whose producer (start) node is visible: if the consumer (end) node is also
/// visible, the link is emitted unchanged; if the consumer is hidden, the graph
/// is walked forward through hidden nodes (DFS, cycle-guarded) until visible
/// input pins are reached, emitting one edge per reached pin. Links starting at
/// a hidden node are skipped (they are reached via the forward walk instead).
/// Hidden nodes with no visible downstream produce no edge. Duplicate edges
/// (same start+end) are collapsed. The model is not modified.
///
/// When a hidden node's `should_bypass` is false, it is *dropped*: the forward
/// walk emits nothing and stops at it, so the node and its links disappear
/// instead of being rerouted. `should_bypass` defaults to "always bypass",
/// preserving the reroute behavior for callers that pass only `is_hidden`.
std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden,
    const std::function<bool(const Node&)>& should_bypass = [](const Node&) { return true; });
