#include "domain/snapshot/factory_snapshot_model.hpp"

#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"

void FactorySnapshotModel::Clear()
{
    nodes.clear();
    links.clear();
    warnings.clear();
    error.clear();
    ok = false;
    flow = ResourceFlowReport{};
}

std::size_t FactorySnapshotModel::CountOfKind(int kind) const
{
    std::size_t count = 0;
    for (const auto& n : nodes)
    {
        if (static_cast<int>(n->GetKind()) == kind) ++count;
    }
    return count;
}
