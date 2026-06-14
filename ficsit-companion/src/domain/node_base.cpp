#include "domain/building.hpp"
#include "domain/game_data.hpp"
#include "domain/link.hpp"
#include "domain/node.hpp"
#include "domain/node_data_resolver.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"

#include <algorithm>
#include <cmath>

#include <imgui_node_editor.h>


Node::Node(const ax::NodeEditor::NodeId id) : id(id)
{

}

Node::Node(const ax::NodeEditor::NodeId id, const Json::Value& serialized) : id(id)
{
    pos.x = serialized["pos"]["x"].get<float>();
    pos.y = serialized["pos"]["y"].get<float>();
}

Node::~Node()
{

}

bool Node::IsPowered() const
{
    return false;
}

bool Node::IsCraft() const
{
    return false;
}

bool Node::IsGroup() const
{
    return false;
}

bool Node::IsOrganizer() const
{
    return false;
}

bool Node::IsMerger() const
{
    return false;
}

bool Node::IsCustomSplitter() const
{
    return false;
}

bool Node::IsGameSplitter() const
{
    return false;
}

bool Node::IsSink() const
{
    return false;
}

bool Node::IsExtractor() const
{
    return false;
}

bool Node::IsLogistics() const
{
    return false;
}

Json::Value Node::Serialize() const
{
    Json::Value node;
    node["kind"] = static_cast<int>(GetKind());
    node["pos"] = {
        { "x", pos.x },
        { "y", pos.y }
    };

    return node;
}

std::unique_ptr<Node> Node::Deserialize(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver)
{
    const Kind kind = static_cast<Node::Kind>(serialized["kind"].get<int>());
    switch (kind)
    {
    case Kind::Craft:
        return std::make_unique<CraftNode>(id, id_generator, serialized, resolver);
    case Kind::Merger:
        return std::make_unique<MergerNode>(id, id_generator, serialized, resolver);
    case Kind::CustomSplitter:
        return std::make_unique<CustomSplitterNode>(id, id_generator, serialized, resolver);
    case Kind::Group:
        return std::make_unique<GroupNode>(id, id_generator, serialized, resolver);
    case Kind::GameSplitter:
        return std::make_unique<GameSplitterNode>(id, id_generator, serialized, resolver);
    case Kind::Sink:
        return std::make_unique<SinkNode>(id, id_generator, serialized, resolver);
    case Kind::Extractor:
        return std::make_unique<ExtractorNode>(id, id_generator, serialized, resolver);
    case Kind::Logistics:
    {
        const auto lk = static_cast<LogisticsNode::Kind>(serialized["logistics_kind"].get<int>());
        if (lk == LogisticsNode::Kind::TruckStation || lk == LogisticsNode::Kind::TrainStation)
        {
            return std::make_unique<VehicleStationNode>(id, id_generator, serialized, resolver);
        }
        return std::make_unique<LogisticsNode>(id, id_generator, serialized, resolver);
    }
    default: // To make compilers happy, but should never happen
        throw std::domain_error("Unimplemented node type in Deserialize");
        return nullptr;
    }

    return nullptr;
}

PoweredNode::PoweredNode(const ax::NodeEditor::NodeId id) : Node(id), current_rate(1, 1), same_clock_power(0, 1), last_underclock_power(0, 1), num_somersloop(0, 1)
{

}

PoweredNode::PoweredNode(const ax::NodeEditor::NodeId id, const Json::Value& serialized) : Node(id, serialized), same_clock_power(0, 1), last_underclock_power(0, 1), num_somersloop(0, 1)
{
    const Kind kind = static_cast<Kind>(serialized["kind"].get<int>());
    if (kind != Kind::Craft && kind != Kind::Group && kind != Kind::Extractor)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a powered node");
    }
    current_rate = FractionalNumber(serialized["rate"]["num"].get<long long int>(), serialized["rate"]["den"].get<long long int>());
}

PoweredNode::~PoweredNode()
{

}

bool PoweredNode::IsPowered() const
{
    return true;
}

Json::Value PoweredNode::Serialize() const
{
    Json::Value node = Node::Serialize();
    node["rate"] = {
        { "num", current_rate.GetNumerator()},
        { "den", current_rate.GetDenominator()}
    };
    node["locked"] =
        (ins.size() > 0 && ins[0]->GetLocked()) ||
        (outs.size() > 0 && outs[0]->GetLocked());
    return node;
}

