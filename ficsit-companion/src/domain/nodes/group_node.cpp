#include "domain/gamedata/building.hpp"
#include "domain/gamedata/game_data.hpp"
#include "domain/graph/graph_item_resolve.hpp"
#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/nodes/node_data_resolver.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include <algorithm>
#include <cmath>

#include <imgui_node_editor.h>


GroupNode::GroupNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator,
    std::vector<std::unique_ptr<Node>>&& nodes_, std::vector<std::unique_ptr<Link>>&& links_) :
    PoweredNode(id), nodes(std::move(nodes_)), links(std::move(links_)), name(""), variable_power(false), loading_error(false)
{
    CreateInsOuts(id_generator);
    ComputePowerUsage();
    UpdateDetails();
}

GroupNode::GroupNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) : PoweredNode(id, serialized)
{
    if (static_cast<Kind>(serialized["kind"].get<int>()) != Kind::Group)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as a group node");
    }
    name = serialized["name"].get_string();

    unsigned long long int current_id = 0;
    auto local_id_generator = [&]() { return current_id++; };

    std::vector<int> node_indices;
    node_indices.reserve(serialized["nodes"].size());
    size_t num_nodes = 0;
    for (const auto& n : serialized["nodes"].get_array())
    {
        try
        {
            nodes.emplace_back(Node::Deserialize(local_id_generator(), local_id_generator, n, resolver));
            node_indices.push_back(num_nodes);
            num_nodes += 1;
        }
        catch (std::exception)
        {
            node_indices.push_back(-1);
        }
    }

    loading_error = false;
    for (const auto& l : serialized["links"].get_array())
    {
        const int start_node_index = l["start"]["node"].get<int>();
        const int end_node_index = l["end"]["node"].get<int>();
        // At least one of the linked node wasn't properly loaded
        if (start_node_index >= node_indices.size() || end_node_index >= node_indices.size() ||
            node_indices[start_node_index] == -1 || node_indices[end_node_index] == -1)
        {
            loading_error = true;
        }

        const Node* start_node = nodes[node_indices[start_node_index]].get();
        const Node* end_node = nodes[node_indices[end_node_index]].get();

        const int start_pin_index = l["start"]["pin"].get<int>();
        const int end_pin_index = l["end"]["pin"].get<int>();

        if (start_pin_index >= start_node->outs.size() || end_pin_index >= end_node->ins.size())
        {
            loading_error = true;
        }

        Pin* start = start_node->outs[start_pin_index].get();
        Pin* end = end_node->ins[end_pin_index].get();
        links.emplace_back(std::make_unique<Link>(local_id_generator(), start, end));
        start->links.push_back(links.back().get());
        end->links.push_back(links.back().get());

        Link* created = links.back().get();
        if (l.contains("rate"))
        {
            created->current_rate = FractionalNumber(
                l["rate"]["num"].get<long long int>(),
                l["rate"]["den"].get<long long int>());
        }
        else
        {
            // Pre-multi-link group: one link per pin, so the edge carried its end pin's rate.
            created->current_rate = end->current_rate;
        }
    }

    // Rebuild route (plug<->plug) links between vehicle stations. Plugs live
    // outside ins/outs, so route links are stored by node index only.
    auto station_plug = [](const Node* n) -> Pin* {
        if (n == nullptr || !n->IsLogistics()) return nullptr;
        auto* lg = static_cast<const LogisticsNode*>(n);
        if (lg->logistics_kind != LogisticsNode::Kind::TruckStation &&
            lg->logistics_kind != LogisticsNode::Kind::TrainStation) return nullptr;
        return static_cast<const VehicleStationNode*>(lg)->plug.get();
    };
    if (serialized.contains("route_links"))
    {
        for (const auto& l : serialized["route_links"].get_array())
        {
            const int s = l["start"].get<int>();
            const int e = l["end"].get<int>();
            if (s < 0 || e < 0 || s >= static_cast<int>(node_indices.size()) ||
                e >= static_cast<int>(node_indices.size()) ||
                node_indices[s] == -1 || node_indices[e] == -1)
            {
                loading_error = true;
                continue;
            }

            Pin* start_plug = station_plug(nodes[node_indices[s]].get());
            Pin* end_plug = station_plug(nodes[node_indices[e]].get());
            if (start_plug == nullptr || end_plug == nullptr)
            {
                loading_error = true;
                continue;
            }

            links.emplace_back(std::make_unique<Link>(local_id_generator(), start_plug, end_plug));
            Link* route_link = links.back().get();
            static_cast<VehicleStationNode*>(start_plug->node)->route_links.push_back(route_link);
            static_cast<VehicleStationNode*>(end_plug->node)->route_links.push_back(route_link);
        }
    }

    CreateInsOuts(id_generator);
    const bool locked = serialized["locked"].get<bool>();
    for (auto& p : ins)
    {
        p->SetLocked(locked);
    }
    for (auto& p : outs)
    {
        p->SetLocked(locked);
    }
    ComputePowerUsage();
    UpdateDetails();
}

Node::Kind GroupNode::GetKind() const
{
    return Kind::Group;
}

bool GroupNode::IsGroup() const
{
    return true;
}

Json::Value GroupNode::Serialize() const
{
    Json::Value node = PoweredNode::Serialize();
    node["name"] = name;

    Json::Array serialized_nodes;
    serialized_nodes.reserve(nodes.size());
    for (const auto& n : nodes)
    {
        serialized_nodes.push_back(n->Serialize());
    }
    node["nodes"] = serialized_nodes;

    auto get_node_index = [&](const Node* node) {
        for (int i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].get() == node)
            {
                return i;
            }
        }
        return -1;
    };

    auto get_pin_index = [&](const Pin* pin) {
        const std::vector<std::unique_ptr<Pin>>& pins = pin->direction == ax::NodeEditor::PinKind::Input ? pin->node->ins : pin->node->outs;
        for (int i = 0; i < pins.size(); ++i)
        {
            if (pins[i].get() == pin)
            {
                return i;
            }
        }
        return -1;
    };

    Json::Array serialized_links;
    serialized_links.reserve(links.size());
    for (const auto& l : links)
    {
        if (IsVehiclePlug(l->start) && IsVehiclePlug(l->end))
        {
            continue;
        }

        const int start_node_index = get_node_index(l->start->node);
        const int end_node_index = get_node_index(l->end->node);
        const int start_pin_index = get_pin_index(l->start);
        const int end_pin_index = get_pin_index(l->end);
        if (start_node_index == -1 || end_node_index == -1 ||
            start_pin_index == -1 || end_pin_index == -1)
        {
            continue;
        }

        serialized_links.push_back({
            { "start", {
                { "node", get_node_index(l->start->node) },
                { "pin", get_pin_index(l->start) }
            }},
            { "end", {
                { "node", get_node_index(l->end->node) },
                { "pin", get_pin_index(l->end) }
            }},
            { "rate", {
                { "num", l->current_rate.GetNumerator() },
                { "den", l->current_rate.GetDenominator() }
            }}
        });
    }
    node["links"] = serialized_links;

    // Route links (plug<->plug) are serialized by node indices only, like
    // SessionSerializer, because plugs live outside ins/outs.
    Json::Array serialized_route_links;
    for (const auto& l : links)
    {
        if (!IsVehiclePlug(l->start) || !IsVehiclePlug(l->end))
        {
            continue;
        }
        const int start_node_index = get_node_index(l->start->node);
        const int end_node_index = get_node_index(l->end->node);
        if (start_node_index == -1 || end_node_index == -1)
        {
            continue;
        }
        serialized_route_links.push_back({
            { "start", start_node_index },
            { "end", end_node_index }
        });
    }
    node["route_links"] = serialized_route_links;

    return node;
}

void GroupNode::UpdateRate(const FractionalNumber& new_rate)
{
    current_rate = new_rate;

    PropagateRateToSubnodes();
    ComputePowerUsage();
    UpdateDetails();

    // Inputs and outputs should always be proportional to the current rate as the group acts as one big CraftNode (I think)
    for (auto& p : ins)
    {
        p->current_rate = p->base_rate * current_rate;
    }
    for (auto& p : outs)
    {
        p->current_rate = p->base_rate * current_rate;
    }
}

bool GroupNode::HasVariablePower() const
{
    return variable_power;
}

void GroupNode::ComputePowerUsage()
{
    same_clock_power = FractionalNumber(0, 1);
    last_underclock_power = FractionalNumber(0, 1);

    variable_power = false;
    for (auto& n : nodes)
    {
        if (n->IsPowered())
        {
            PoweredNode* powered = static_cast<CraftNode*>(n.get());
            powered->ComputePowerUsage();
            same_clock_power += powered->same_clock_power;
            last_underclock_power += powered->last_underclock_power;
            variable_power |= powered->HasVariablePower();
        }
    }
}

void GroupNode::PropagateRateToSubnodes()
{
    inputs.clear();
    outputs.clear();

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        // Update powered node with current rate to get proper power
        if (nodes[i]->IsPowered())
        {
            static_cast<PoweredNode*>(nodes[i].get())->UpdateRate(nodes_base_rate[i] * current_rate);

            if (nodes[i]->IsCraft())
            {
                for (auto& p : nodes[i]->ins)
                {
                    inputs[p->item] += p->current_rate;
                }
                for (auto& p : nodes[i]->outs)
                {
                    outputs[p->item] += p->current_rate;
                }
            }
            else if (nodes[i]->IsGroup())
            {
                const GroupNode* node = static_cast<const GroupNode*>(nodes[i].get());
                for (const auto& [k, v] : node->inputs)
                {
                    inputs[k] += v;
                }
                for (const auto& [k, v] : node->outputs)
                {
                    outputs[k] += v;
                }
            }
        }
        else if (nodes[i]->IsSink())
        {
            for (auto& p : nodes[i]->ins)
            {
                if (p->item != nullptr)
                {
                    inputs[p->item] += p->current_rate * current_rate;
                }
            }
        }
        // Don't update organizer nodes so the current_rate actually stores the base_rate
        // (this prevents the information to be lost when group rate is set to 0)
    }
}

void GroupNode::SetBuiltState(const bool b)
{
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (nodes[i]->IsCraft())
        {
            static_cast<CraftNode*>(nodes[i].get())->built = b;
        }
        else if (nodes[i]->IsGroup())
        {
            static_cast<GroupNode*>(nodes[i].get())->SetBuiltState(b);
        }
    }
    UpdateDetails();
}

void GroupNode::CreateInsOuts(const std::function<unsigned long long int()>& id_generator)
{
    inputs.clear();
    outputs.clear();
    nodes_base_rate.reserve(nodes.size());
    for (auto& n : nodes)
    {
        if (n->IsCraft())
        {
            for (auto& p : n->ins)
            {
                inputs[p->item] += p->current_rate;
            }
            for (auto& p : n->outs)
            {
                outputs[p->item] += p->current_rate;
            }
        }
        else if (n->IsGroup())
        {
            const GroupNode* node = static_cast<const GroupNode*>(n.get());
            for (const auto& [k, v] : node->inputs)
            {
                inputs[k] += v;
            }
            for (const auto& [k, v] : node->outputs)
            {
                outputs[k] += v;
            }
        }
        // Add all sink inputs as required additional inputs
        else if (n->IsSink())
        {
            for (auto& p : n->ins)
            {
                if (p->item != nullptr)
                {
                    inputs[p->item] += p->current_rate * current_rate;
                }
            }
        }

        if (n->IsPowered())
        {
            nodes_base_rate.push_back(static_cast<PoweredNode*>(n.get())->current_rate);
        }
        else
        {
            nodes_base_rate.push_back(FractionalNumber(0, 1));
        }
    }

    // Create input pins for resources required in the group
    for (const auto& [k, v] : inputs)
    {
        const auto it = outputs.find(k);
        // Only consumed, create an input pin
        if (it == outputs.end())
        {
            ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, k, false, v));
            ins.back()->current_rate = v;
        }
        // Less produced than consumed, create an input pin with the difference
        else if (it->second < v)
        {
            ins.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Input, this, k, false, v - it->second));
            ins.back()->current_rate = v - it->second;
        }
    }

    // Create output pins for resources overproduced in the group
    for (const auto& [k, v] : outputs)
    {
        const auto it = inputs.find(k);
        // Only produced, create an output pin
        if (it == inputs.end())
        {
            outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, k, false, v));
            outs.back()->current_rate = v;
        }
        // Less consumed than produced, create an output pin with the difference
        else if (it->second < v)
        {
            outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, k, false, v - it->second));
            outs.back()->current_rate = v - it->second;
        }
    }
}

void GroupNode::UpdateDetails()
{
    total_machines = {};
    built_machines = {};
    detailed_machines = {};
    detailed_machine_outputs = {};
    detailed_power_same_clock = {};
    detailed_power_last_underclock = {};
    detailed_sinked_points = {};
    num_somersloop = FractionalNumber(0, 1);
    for (const auto& n : nodes)
    {
        if (n->IsCraft())
        {
            const CraftNode* node = static_cast<const CraftNode*>(n.get());
            total_machines[node->recipe->building->name] += node->current_rate;
            built_machines[node->recipe->building->name] += node->built ? node->current_rate : 0;
            detailed_machines[node->recipe->building->name][node->recipe] += node->current_rate;
            detailed_power_same_clock[node->recipe] += node->same_clock_power;
            detailed_power_last_underclock[node->recipe] += node->last_underclock_power;
            num_somersloop += node->num_somersloop * static_cast<int>(std::ceil((node->current_rate / FractionalNumber(5, 2)).GetValue()));
        }
        else if (n->IsGroup())
        {
            const GroupNode* node = static_cast<const GroupNode*>(n.get());
            for (const auto& [k, v] : node->total_machines)
            {
                total_machines[k] += v;
            }
            for (const auto& [k, v] : node->built_machines)
            {
                built_machines[k] += v;
            }
            for (const auto& [k, v] : node->detailed_machines)
            {
                for (const auto& [k2, v2] : v)
                {
                    detailed_machines[k][k2] += v2;
                }
            }
            for (const auto& [k, v] : node->detailed_machine_outputs)
            {
                for (const auto& [k2, v2] : v)
                {
                    detailed_machine_outputs[k][k2] += v2;
                }
            }
            for (const auto& [k, v] : node->detailed_power_same_clock)
            {
                detailed_power_same_clock[k] += v;
            }
            for (const auto& [k, v] : node->detailed_power_last_underclock)
            {
                detailed_power_last_underclock[k] += v;
            }
            num_somersloop += node->num_somersloop;
        }
        else if (n->IsSink())
        {
            for (const auto& p : n->ins)
            {
                if (p->item != nullptr)
                {
                    detailed_sinked_points[p->item] = p->current_rate * current_rate * p->item->sink_value;
                }
            }
        }
        else if (n->IsExtractor())
        {
            const ExtractorNode* node = static_cast<const ExtractorNode*>(n.get());
            const Building* building = node->GetBuilding();
            if (building != nullptr)
            {
                total_machines[building->name] += node->current_rate;
                for (const auto& p : n->outs)
                {
                    if (p->item != nullptr)
                    {
                        detailed_machine_outputs[building->name][p->item] += p->current_rate;
                    }
                }
                variable_power |= building->variable_power;
            }
        }
        else if (n->IsLogistics())
        {
            const LogisticsNode* node = static_cast<const LogisticsNode*>(n.get());
            total_machines[node->GetDisplayName()] += 1;
            built_machines[node->GetDisplayName()] += 0;
        }
    }
}
