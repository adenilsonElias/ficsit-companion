#pragma once
#include <imgui_node_editor.h>
#include <imgui.h>

/// @brief Seam over the imgui-node-editor free functions used by non-render
/// logic, so that logic can be unit-tested without a live editor context.
class IEditorBackend
{
public:
    virtual ~IEditorBackend() = default;
    virtual void DeleteNode(ax::NodeEditor::NodeId id) = 0;
    virtual void DeleteLink(ax::NodeEditor::LinkId id) = 0;
    virtual void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) = 0;
    virtual ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) = 0;
    /// @brief Style flow duration, used when flagging a propagation error.
    virtual float GetFlowDuration() = 0;
};

/// @brief Forwards to the real ax::NodeEditor functions (requires a live editor context at call time).
class NodeEditorBackend : public IEditorBackend
{
public:
    void DeleteNode(ax::NodeEditor::NodeId id) override;
    void DeleteLink(ax::NodeEditor::LinkId id) override;
    void SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) override;
    ImVec2 GetNodePosition(ax::NodeEditor::NodeId id) override;
    float GetFlowDuration() override;
};
