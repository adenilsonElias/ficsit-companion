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


SinkNode::SinkNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item) : Node(id)
{
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
}

SinkNode::SinkNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : Node(id, serialized)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::Sink)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a sink node");
    }

    if (serialized["ins"].size() == 0)
    {
        throw std::runtime_error("Trying to deserialize an unvalid sink node (wrong number of inputs)");
    }
    for (const auto& i : serialized["ins"].get_array())
    {
        const Item* item = resolver.FindItem(i["item"].get_string());
        if (item == nullptr && i["item"].get_string() != "")
        {
            throw std::runtime_error("Unknown item when loading sink node");
        }
        ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item, i["locked"].get<bool>()));
        ins.back()->current_rate = FractionalNumber(i["num"].get<long long int>(), i["den"].get<long long int>());
    }
}

SinkNode::~SinkNode()
{

}

bool SinkNode::IsSink() const
{
    return true;
}

Node::Kind SinkNode::GetKind() const
{
    return Node::Kind::Sink;
}

Json::Value SinkNode::Serialize() const
{
    Json::Value serialized = Node::Serialize();

    Json::Array ins_array;
    ins_array.reserve(ins.size());
    for (auto& i : ins)
    {
        ins_array.push_back({
            { "num", i->current_rate.GetNumerator() },
            { "den", i->current_rate.GetDenominator() },
            { "item", i->item == nullptr ? "" : i->item->name },
            { "locked", i->GetLocked() },
        });
    }
    serialized["ins"] = ins_array;

    return serialized;
}
