#include <catch2/catch_test_macros.hpp>
#include "graph_test_helpers.hpp"

/// @test   The headless IEditorBackend test double stores a node position and reads it back, and
///         records node deletions so logic that calls into the editor can be driven without a live
///         node-editor context.
/// @covers FakeEditorBackend::SetNodePosition/GetNodePosition (round-trip of an ImVec2) and
///         DeleteNode (appending to the recorded-deletions list). Guards the seam every other
///         graph test relies on to run without ImGui.
TEST_CASE("FakeEditorBackend records deletions and positions", "[editor_backend]")
{
    FakeEditorBackend fake;
    fake.SetNodePosition(ax::NodeEditor::NodeId(7), ImVec2(3.0f, 4.0f));
    ImVec2 p = fake.GetNodePosition(ax::NodeEditor::NodeId(7));
    REQUIRE(p.x == 3.0f);
    REQUIRE(p.y == 4.0f);
    fake.DeleteNode(ax::NodeEditor::NodeId(7));
    REQUIRE(fake.deleted_nodes.size() == 1);
}
