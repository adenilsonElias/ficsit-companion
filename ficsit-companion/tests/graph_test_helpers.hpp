#pragma once
#include <map>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include "infra/ports/editor_backend.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/graph/link.hpp"

/// @brief Records calls and stores positions so headless tests can drive logic
/// that would otherwise need a live node-editor context.
class FakeEditorBackend : public IEditorBackend
{
public:
    void DeleteNode(ax::NodeEditor::NodeId id) override { deleted_nodes.push_back(id); }
    void DeleteLink(ax::NodeEditor::LinkId id) override { deleted_links.push_back(id); }
    void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) override { positions[id.Get()] = pos; }
    ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) override
    {
        auto it = positions.find(id.Get());
        return it == positions.end() ? ImVec2(0, 0) : it->second;
    }
    float GetFlowDuration() override { return flow_duration; }

    std::vector<ax::NodeEditor::NodeId> deleted_nodes;
    std::vector<ax::NodeEditor::LinkId> deleted_links;
    std::map<uintptr_t, ImVec2> positions;
    float flow_duration = 1.0f;
};

/// @brief Monotonic id generator standing in for ProductionApp::GetNextId.
struct IdGen {
    unsigned long long n = 1;
    unsigned long long operator()() { return n++; }
};

/// @brief Link an output pin to an input pin (no item/rate side effects),
/// for solver tests that only care about flow math.
inline std::unique_ptr<Link> MakeLink(unsigned long long id, Pin* out_pin, Pin* in_pin)
{
    auto link = std::make_unique<Link>(ax::NodeEditor::LinkId(id), out_pin, in_pin);
    out_pin->link = link.get();
    in_pin->link = link.get();
    return link;
}
