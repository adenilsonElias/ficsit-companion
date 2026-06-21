#pragma once

#include <memory>
#include <string>
#include <vector>

#include "domain/resource_flow.hpp"

struct Node;
struct Link;

/// @brief Read-only imported factory state from a `.sav`. Owns its node/link
/// graph (built via the importer), the import warnings, and a cached per-item
/// resource-flow report. Pure data — no UI, no infra dependency. Mutation of
/// the graph is intentionally not exposed; the only state transition is Clear()
/// plus whole-graph assignment by a builder in the infra layer.
struct FactorySnapshotModel
{
    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    std::vector<std::string> warnings;
    /// @brief Non-empty when the last build failed; `ok` is then false.
    std::string error;
    bool ok = false;
    /// @brief Per-item produced/consumed/net report over `nodes`.
    ResourceFlowReport flow;

    /// @brief Reset to empty (no nodes/links/warnings, ok=false, empty report).
    void Clear();

    /// @brief Count of imported nodes whose GetKind() equals `kind`.
    std::size_t CountOfKind(int kind) const;
};
