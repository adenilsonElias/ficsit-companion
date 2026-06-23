#include "infra/ports/editor_backend.hpp"

void NodeEditorBackend::DeleteNode(ax::NodeEditor::NodeId id) { ax::NodeEditor::DeleteNode(id); }
void NodeEditorBackend::DeleteLink(ax::NodeEditor::LinkId id) { ax::NodeEditor::DeleteLink(id); }
void NodeEditorBackend::SetNodePosition(ax::NodeEditor::NodeId id, ImVec2 pos) { ax::NodeEditor::SetNodePosition(id, pos); }
ImVec2 NodeEditorBackend::GetNodePosition(ax::NodeEditor::NodeId id) { return ax::NodeEditor::GetNodePosition(id); }
float NodeEditorBackend::GetFlowDuration() { return ax::NodeEditor::GetStyle().FlowDuration; }
