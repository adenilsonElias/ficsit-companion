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

CraftNode::CraftNode(const ax::NodeEditor::NodeId id, const Recipe* recipe, const std::function<unsigned long long int()>& id_generator) :
    PoweredNode(id), built(false)
{
    ChangeRecipe(recipe, id_generator);
}

CraftNode::CraftNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) :
    PoweredNode(id, serialized)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::Craft)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a craft node");
    }
    recipe = nullptr;
    const std::string& recipe_name = serialized["recipe"].get_string();
    const Recipe* found = resolver.FindRecipe(recipe_name);

    if (found != nullptr)
    {
        ChangeRecipe(found, id_generator);
    }
    else
    {
        throw std::runtime_error("Unknown recipe when loading craft node");
    }

    num_somersloop = FractionalNumber(serialized["num_somersloop"].get<long long int>());
    ComputePowerUsage();
    const bool locked = serialized["locked"].get<bool>();
    for (auto& p : ins)
    {
        p->current_rate = p->base_rate * current_rate;
        p->SetLocked(locked);
    }
    for (auto& p : outs)
    {
        p->current_rate = p->base_rate * current_rate * (1 + num_somersloop * recipe->building->somersloop_mult);
        p->SetLocked(locked);
    }
    built = serialized["built"].get<bool>();
}

CraftNode::~CraftNode()
{

}

bool CraftNode::IsCraft() const
{
    return true;
}

Json::Value CraftNode::Serialize() const
{
    Json::Value node = PoweredNode::Serialize();

    node["recipe"] = recipe->name;
    node["num_somersloop"] = num_somersloop.GetNumerator();
    node["built"] = built;

    return node;
}

void CraftNode::UpdateRate(const FractionalNumber& new_rate)
{
    current_rate = new_rate;
    for (auto& p : ins)
    {
        p->current_rate = p->base_rate * current_rate;
    }
    for (auto& p : outs)
    {
        p->current_rate = p->base_rate * current_rate * (1 + (num_somersloop * recipe->building->somersloop_mult));
    }
    ComputePowerUsage();
}

bool CraftNode::HasVariablePower() const
{
    return recipe->building->variable_power;
}

void CraftNode::ComputePowerUsage()
{
    const Building* building = recipe->building;
    // All machines are underclocked at current_rate/num_machines
    const int num_machines = static_cast<int>(std::ceil(current_rate.GetValue()));
    const double power = recipe->power;
    double same_clock_power_double =
        num_machines *
        power *
        std::pow(1.0 + num_somersloop.GetValue() * building->somersloop_mult.GetValue(), building->somersloop_power_exponent) *
        std::pow(current_rate.GetValue() / static_cast<double>(std::max(1, num_machines)), building->power_exponent);
    // num_full_machines at 100% rate + one extra underclocked machine
    const int num_full_machines = static_cast<int>(std::floor(current_rate.GetValue()));
    double last_underclock_power_double =
        num_full_machines *
        power *
        std::pow(1.0 + num_somersloop.GetValue() * building->somersloop_mult.GetValue(), building->somersloop_power_exponent);
    last_underclock_power_double +=
        power *
        std::pow(1.0 + num_somersloop.GetValue() * building->somersloop_mult.GetValue(), building->somersloop_power_exponent) *
        std::pow(current_rate.GetValue() - num_full_machines, building->power_exponent);
    // Round values at 0.001 precision for the power, as we don't have exact fractional values with the exponents anyway
    same_clock_power = FractionalNumber(static_cast<long long int>(std::round(same_clock_power_double * 1000.0)), 1000);
    last_underclock_power = FractionalNumber(static_cast<long long int>(std::round(last_underclock_power_double * 1000.0)), 1000);

}

void CraftNode::ChangeRecipe(const Recipe* recipe, const std::function<unsigned long long int()>& id_generator)
{
    this->recipe = recipe;
    if (recipe == nullptr)
    {
        ins.clear();
        outs.clear();
        return;
    }

    for (const auto& input : recipe->ins)
    {
        ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, input.item, false, input.quantity));
    }
    for (const auto& output : recipe->outs)
    {
        outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, output.item, false, output.quantity));
    }
    UpdateRate(current_rate);
}

Node::Kind CraftNode::GetKind() const
{
    return Node::Kind::Craft;
}
