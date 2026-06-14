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


OrganizerNode::OrganizerNode(const ax::NodeEditor::NodeId id, const Item* item) : Node(id), item(item)
{

}

OrganizerNode::OrganizerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : Node(id, serialized), item(nullptr)
{
    const Kind kind = static_cast<Kind>(serialized["kind"].get<int>());
    if (kind != Kind::Merger && kind != Kind::CustomSplitter && kind != Kind::GameSplitter)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as an organizer node");
    }
    item = resolver.FindItem(serialized["item"].get_string());
    if (item == nullptr && serialized["item"].get_string() != "")
    {
        throw std::runtime_error("Unknown item when loading organizer node");
    }

    for (size_t i = 0; i < serialized["ins"].size(); ++i)
    {
        ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item, serialized["ins"][i]["locked"].get<bool>()));
        ins.back()->current_rate = FractionalNumber(serialized["ins"][i]["num"].get<long long int>(), serialized["ins"][i]["den"].get<long long int>());
    }

    for (size_t i = 0; i < serialized["outs"].size(); ++i)
    {
        outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item, serialized["outs"][i]["locked"].get<bool>()));
        outs.back()->current_rate = FractionalNumber(serialized["outs"][i]["num"].get<long long int>(), serialized["outs"][i]["den"].get<long long int>());
    }
}

OrganizerNode::~OrganizerNode()
{

}

bool OrganizerNode::IsOrganizer() const
{
    return true;
}

Json::Value OrganizerNode::Serialize() const
{
    Json::Value serialized = Node::Serialize();

    serialized["item"] = item == nullptr ? "" : item->name;

    Json::Array ins_array;
    ins_array.reserve(ins.size());
    for (auto& i : ins)
    {
        ins_array.push_back({
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
            { "num", o->current_rate.GetNumerator() },
            { "den", o->current_rate.GetDenominator() },
            { "locked", o->GetLocked() },
        });
    }
    serialized["outs"] = outs_array;

    return serialized;
}

void OrganizerNode::ChangeItem(const Item* item)
{
    this->item = item;
    for (auto& p : ins)
    {
        p->item = item;
    }
    for (auto& p : outs)
    {
        p->item = item;
    }
}

void OrganizerNode::RemoveItemIfNotForced()
{
    if (item == nullptr)
    {
        return;
    }

    for (auto& p : ins)
    {
        if (p->link != nullptr)
        {
            return;
        }
    }

    for (auto& p : outs)
    {
        if (p->link != nullptr)
        {
            return;
        }
    }

    ChangeItem(nullptr);
}

bool OrganizerNode::IsBalanced() const
{
    FractionalNumber input_sum(0, 1);
    for (const auto& p : ins)
    {
        input_sum += p->current_rate;
    }
    FractionalNumber output_sum(0, 1);
    for (const auto& p : outs)
    {
        output_sum += p->current_rate;
    }

    return input_sum == output_sum;
}

CustomSplitterNode::CustomSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item) : OrganizerNode(id, item)
{
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
}

CustomSplitterNode::CustomSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : OrganizerNode(id, id_generator, serialized, resolver)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::CustomSplitter)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a custom splitter node");
    }

    if (serialized["ins"].size() != 1)
    {
        throw std::runtime_error("Trying to deserialize an unvalid custom splitter node (wrong number of inputs)");
    }
}

CustomSplitterNode::~CustomSplitterNode()
{
}

bool CustomSplitterNode::IsCustomSplitter() const
{
    return true;
}

Node::Kind CustomSplitterNode::GetKind() const
{
    return Node::Kind::CustomSplitter;
}

MergerNode::MergerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item) : OrganizerNode(id, item)
{
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
}

MergerNode::MergerNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : OrganizerNode(id, id_generator, serialized, resolver)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::Merger)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a merger node");
    }

    if (serialized["outs"].size() != 1)
    {
        throw std::runtime_error("Trying to deserialize an unvalid merger node (wrong number of outputs)");
    }
}

MergerNode::~MergerNode()
{

}

bool MergerNode::IsMerger() const
{
    return true;
}

Node::Kind MergerNode::GetKind() const
{
    return Node::Kind::Merger;
}

GameSplitterNode::GameSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Item* item) : OrganizerNode(id, item)
{
    ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, item));
}

GameSplitterNode::GameSplitterNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : OrganizerNode(id, id_generator, serialized, resolver)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::GameSplitter)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a game splitter node");
    }

    if (serialized["ins"].size() != 1)
    {
        throw std::runtime_error("Trying to deserialize an unvalid game splitter node (wrong number of inputs)");
    }
}

GameSplitterNode::~GameSplitterNode()
{
}

bool GameSplitterNode::IsGameSplitter() const
{
    return true;
}

Node::Kind GameSplitterNode::GetKind() const
{
    return Node::Kind::GameSplitter;
}

bool GameSplitterNode::IsBalanced() const
{
    if (outs.size() == 0)
    {
        return OrganizerNode::IsBalanced();
    }

    for (const auto& p : outs)
    {
        if (p->current_rate != outs[0]->current_rate)
        {
            return false;
        }
    }
    return OrganizerNode::IsBalanced();
}
