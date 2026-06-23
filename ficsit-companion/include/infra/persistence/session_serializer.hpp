#pragma once

#include <string>

class GraphModel;
class IEditorBackend;

class SessionSerializer
{
public:
    SessionSerializer(GraphModel& graph, IEditorBackend& editor, int save_version);

    std::string Serialize() const;
    void Deserialize(const std::string& s);

private:
    GraphModel& graph;
    IEditorBackend& editor;
    int save_version;
};
