#include "domain/snapshot/snapshot_bypass.hpp"

#include "domain/graph/link.hpp"
#include "domain/graph/pin.hpp"
#include "domain/nodes/node.hpp"

#include <set>
#include <unordered_set>
#include <utility>

namespace
{
    // Walk forward from a hidden node, collecting the ids of every visible input
    // pin reachable through chains of hidden nodes. `visited` guards cycles.
    void CollectVisibleSinks(const Node& hidden_node,
                             const std::function<bool(const Node&)>& is_hidden,
                             std::unordered_set<const Node*>& visited,
                             std::vector<ax::NodeEditor::PinId>& sinks)
    {
        if (!visited.insert(&hidden_node).second) return;
        for (const auto& out : hidden_node.outs)
        {
            if (!out || !out->link) continue;
            const Pin* end = out->link->end;
            if (!end || !end->node) continue;
            if (is_hidden(*end->node))
                CollectVisibleSinks(*end->node, is_hidden, visited, sinks);
            else
                sinks.push_back(end->id);
        }
    }
}

std::vector<SnapshotEdge> ComputeVisibleEdges(
    const std::vector<std::unique_ptr<Node>>& /*nodes*/,
    const std::vector<std::unique_ptr<Link>>& links,
    const std::function<bool(const Node&)>& is_hidden)
{
    std::vector<SnapshotEdge> edges;
    std::set<std::pair<uintptr_t, uintptr_t>> seen; // dedupe (start,end) pin ids

    auto emit = [&](ax::NodeEditor::PinId start, ax::NodeEditor::PinId end)
    {
        if (seen.insert({ start.Get(), end.Get() }).second)
            edges.push_back({ start, end });
    };

    for (const auto& link : links)
    {
        if (!link || !link->start || !link->end) continue;
        const Pin* start = link->start;
        const Pin* end = link->end;
        if (!start->node || !end->node) continue;
        if (is_hidden(*start->node)) continue; // reached via forward walk instead

        if (!is_hidden(*end->node))
        {
            emit(start->id, end->id);
        }
        else
        {
            std::unordered_set<const Node*> visited;
            std::vector<ax::NodeEditor::PinId> sinks;
            CollectVisibleSinks(*end->node, is_hidden, visited, sinks);
            for (const auto& sink : sinks) emit(start->id, sink);
        }
    }
    return edges;
}
