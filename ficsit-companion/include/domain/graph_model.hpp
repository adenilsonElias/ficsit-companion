#pragma once

#include <memory>
#include <vector>

#include <imgui_node_editor.h>

#include "domain/fractional_number.hpp"

struct Link;
struct Node;
struct Pin;
class IEditorBackend;

class GraphModel
{
public:
    explicit GraphModel(IEditorBackend& editor);

    unsigned long long int GetNextId();
    Pin* FindPin(ax::NodeEditor::PinId id) const;
    void CreateLink(Pin* start, Pin* end, bool trigger_update, float& error_time, float error_flow_duration);
    void DeleteLink(ax::NodeEditor::LinkId id);
    void DeleteNode(ax::NodeEditor::NodeId id);

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<std::unique_ptr<Link>> links;
    unsigned long long int next_id = 1;

private:
    IEditorBackend& editor;
};
