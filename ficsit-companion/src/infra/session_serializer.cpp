#include "infra/session_serializer.hpp"

#include "infra/editor_backend.hpp"
#include "domain/game_data.hpp"
#include "domain/graph_item_resolve.hpp"
#include "domain/graph_model.hpp"
#include "domain/json.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/pin.hpp"

#include <cstdio>
#include <memory>
#include <vector>

SessionSerializer::SessionSerializer(GraphModel& graph, IEditorBackend& editor, int save_version)
    : graph(graph)
    , editor(editor)
    , save_version(save_version)
{
}

std::string SessionSerializer::Serialize() const
{
    Json::Value output;
    output["save_version"] = save_version;
    output["game_version"] = Data::Version();

    Json::Array saved_nodes;
    saved_nodes.reserve(graph.nodes.size());
    for (const auto& n : graph.nodes)
    {
        saved_nodes.push_back(n->Serialize());
    }
    output["nodes"] = saved_nodes;


    auto get_node_index = [&](const Node* n) -> int {
        for (int i = 0; i < graph.nodes.size(); ++i)
        {
            if (graph.nodes[i].get() == n)
            {
                return i;
            }
        }
        return -1;
    };

    auto get_pin_index = [&](const Pin* p) -> int {
        const std::vector<std::unique_ptr<Pin>>& pins = p->direction == ax::NodeEditor::PinKind::Output ? p->node->outs : p->node->ins;
        for (int i = 0; i < pins.size(); ++i)
        {
            if (pins[i].get() == p)
            {
                return i;
            }
        }
        return -1;
    };

    // A route link connects two vehicle-station plugs (held outside ins/outs);
    // it is serialized by node indices only, in a separate array.
    Json::Array saved_links;
    Json::Array saved_route_links;
    saved_links.reserve(graph.links.size());
    for (const auto& l : graph.links)
    {
        if (IsVehiclePlug(l->start) && IsVehiclePlug(l->end))
        {
            saved_route_links.push_back({
                { "start", get_node_index(l->start->node) },
                { "end", get_node_index(l->end->node) },
            });
            continue;
        }
        saved_links.push_back({
            { "start", {
                { "node", get_node_index(l->start->node) },
                { "pin", get_pin_index(l->start) }
            }},
            { "end", {
                { "node", get_node_index(l->end->node) },
                { "pin", get_pin_index(l->end) }
            }}
        });
    }
    output["links"] = saved_links;
    output["route_links"] = saved_route_links;

    return output.Dump();
}

void SessionSerializer::Deserialize(const std::string& s)
{
    Json::Value content = Json::Parse(s);
    if (content.is_null() || content.size() == 0)
    {
        return;
    }

    if (!UpdateSave(content, save_version))
    {
        printf("Save format not supported with this version (%i VS %i)", content["save_version"].get<int>(), save_version);
        return;
    }

    // Clean current content
    for (const auto& n : graph.nodes)
    {
        editor.DeleteNode(n->id);
    }
    graph.nodes.clear();

    for (const auto& l : graph.links)
    {
        editor.DeleteLink(l->id);
    }
    graph.links.clear();

    // Load nodes
    std::vector<int> node_indices;
    node_indices.reserve(content["nodes"].size());
    size_t num_nodes = 0;
    for (const auto& n : content["nodes"].get_array())
    {
        try
        {
            graph.nodes.emplace_back(Node::Deserialize(graph.GetNextId(), [this] { return graph.GetNextId(); }, n, GameDataResolver()));
            editor.SetNodePosition(graph.nodes.back()->id, graph.nodes.back()->pos);
            node_indices.push_back(static_cast<int>(num_nodes));
            num_nodes += 1;
        }
        catch (std::exception)
        {
            node_indices.push_back(-1);
        }
    }

    // Load links
    for (const auto& l : content["links"].get_array())
    {
        const int start_node_index = l["start"]["node"].get<int>();
        const int end_node_index = l["end"]["node"].get<int>();
        // At least one of the linked node wasn't properly loaded
        if (start_node_index >= node_indices.size() || end_node_index >= node_indices.size() ||
            node_indices[start_node_index] == -1 || node_indices[end_node_index] == -1)
        {
            continue;
        }

        const Node* start_node = graph.nodes[node_indices[start_node_index]].get();
        const Node* end_node = graph.nodes[node_indices[end_node_index]].get();

        const int start_pin_index = l["start"]["pin"].get<int>();
        const int end_pin_index = l["end"]["pin"].get<int>();

        if (start_pin_index >= start_node->outs.size() || end_pin_index >= end_node->ins.size())
        {
            continue;
        }

        float dummy_error_time = 0.0f;
        graph.CreateLink(start_node->outs[start_pin_index].get(), end_node->ins[end_pin_index].get(), false, dummy_error_time, 0.0f);
    }

    // Rebuild route (plug<->plug) links between vehicle stations.
    auto station_plug = [](const Node* n) -> Pin* {
        if (n == nullptr || !n->IsLogistics()) return nullptr;
        auto* lg = static_cast<const LogisticsNode*>(n);
        if (lg->logistics_kind != LogisticsNode::Kind::TruckStation &&
            lg->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
        return static_cast<const VehicleStationNode*>(lg)->plug.get();
    };
    if (content.contains("route_links"))
    {
        for (const auto& l : content["route_links"].get_array())
        {
            const int start_node_index = l["start"].get<int>();
            const int end_node_index = l["end"].get<int>();
            if (start_node_index >= node_indices.size() || end_node_index >= node_indices.size() ||
                node_indices[start_node_index] == -1 || node_indices[end_node_index] == -1)
            {
                continue;
            }
            Pin* start_plug = station_plug(graph.nodes[node_indices[start_node_index]].get());
            Pin* end_plug = station_plug(graph.nodes[node_indices[end_node_index]].get());
            if (start_plug == nullptr || end_plug == nullptr)
            {
                continue;
            }
            float dummy_error_time = 0.0f;
            graph.CreateLink(start_plug, end_plug, false, dummy_error_time, 0.0f);
        }
    }
}
