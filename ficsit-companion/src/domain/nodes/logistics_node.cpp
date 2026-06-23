#include "domain/gamedata/building.hpp"
#include "domain/gamedata/game_data.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/nodes/node_data_resolver.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include <algorithm>
#include <cmath>

#include <imgui_node_editor.h>


LogisticsNode::LogisticsNode(const ax::NodeEditor::NodeId id, LogisticsNode::Kind logistics_kind,
    size_t input_count, size_t output_count,
    const std::function<unsigned long long int()>& id_generator) :
    Node(id), logistics_kind(logistics_kind)
{
    input_count = std::max<size_t>(1, input_count);
    // Dimensional Depot uploads items to a global pool and has no belt output.
    // For every other logistics kind we keep a floor of one output pin.
    const size_t min_outputs = logistics_kind == Kind::DimensionalDepot ? 0 : 1;
    output_count = std::max<size_t>(min_outputs, output_count);
    for (size_t i = 0; i < input_count; ++i)
    {
        ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, nullptr));
    }
    for (size_t i = 0; i < output_count; ++i)
    {
        outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, nullptr));
    }
}

LogisticsNode::LogisticsNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
    const Json::Value& serialized, const INodeDataResolver& resolver) : Node(id, serialized)
{
    if (static_cast<Node::Kind>(serialized["kind"].get<int>()) != Node::Kind::Logistics)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a logistics node");
    }

    logistics_kind = static_cast<LogisticsNode::Kind>(serialized["logistics_kind"].get<int>());
    for (const auto& i : serialized["ins"].get_array())
    {
        const Item* item = resolver.FindItem(i["item"].get_string());
        if (item == nullptr && i["item"].get_string() != "")
        {
            throw std::runtime_error("Unknown item when loading logistics node");
        }
        ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item, i["locked"].get<bool>()));
        ins.back()->current_rate = FractionalNumber(i["num"].get<long long int>(), i["den"].get<long long int>());
    }
    for (const auto& o : serialized["outs"].get_array())
    {
        const Item* item = resolver.FindItem(o["item"].get_string());
        if (item == nullptr && o["item"].get_string() != "")
        {
            throw std::runtime_error("Unknown item when loading logistics node");
        }
        outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item, o["locked"].get<bool>()));
        outs.back()->current_rate = FractionalNumber(o["num"].get<long long int>(), o["den"].get<long long int>());
    }
}

LogisticsNode::~LogisticsNode()
{
}

bool LogisticsNode::IsLogistics() const
{
    return true;
}

Node::Kind LogisticsNode::GetKind() const
{
    return Node::Kind::Logistics;
}

Json::Value LogisticsNode::Serialize() const
{
    Json::Value serialized = Node::Serialize();
    serialized["logistics_kind"] = static_cast<int>(logistics_kind);

    Json::Array ins_array;
    ins_array.reserve(ins.size());
    for (auto& i : ins)
    {
        ins_array.push_back({
            { "item", i->item == nullptr ? "" : i->item->name },
            { "num", i->current_rate.GetNumerator() },
            { "den", i->current_rate.GetDenominator() },
            { "locked", i->GetLocked() },
        });
    }
    serialized["ins"] = ins_array;

    Json::Array outs_array;
    outs_array.reserve(outs.size());
    for (auto& o : outs)
    {
        outs_array.push_back({
            { "item", o->item == nullptr ? "" : o->item->name },
            { "num", o->current_rate.GetNumerator() },
            { "den", o->current_rate.GetDenominator() },
            { "locked", o->GetLocked() },
        });
    }
    serialized["outs"] = outs_array;

    return serialized;
}

const char* LogisticsNode::GetDisplayName() const
{
    switch (logistics_kind)
    {
    case LogisticsNode::Kind::Storage:
        return "Storage";
    case LogisticsNode::Kind::IndustrialStorage:
        return "Industrial Storage";
    case LogisticsNode::Kind::TruckStation:
        return "Truck Station";
    case LogisticsNode::Kind::TrainStation:
        return "Train Station";
    case LogisticsNode::Kind::DimensionalDepot:
        return "Dimensional Depot";
    case LogisticsNode::Kind::PipeJunction:
        return "Pipe Junction";
    case LogisticsNode::Kind::FluidBuffer:
        return "Fluid Buffer";
    case LogisticsNode::Kind::IndustrialFluidBuffer:
        return "Industrial Fluid Buffer";
    default:
        return "Logistics";
    }
}

