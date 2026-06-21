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

// ---------------------------------------------------------------------------
// ExtractorNode
// ---------------------------------------------------------------------------

namespace
{
    struct ExtractorSpec
    {
        const char* building_name;
        long long base_rate_num;
        long long base_rate_den;
        bool supports_purity;
    };

    // Indexed by ExtractorNode::Kind. Base rates in items/min (or m^3/min) at
    // 100% clock and Normal purity. Sourced from Satisfactory 1.0 wiki.
    constexpr ExtractorSpec kExtractorSpecs[] = {
        /* MinerMk1       */ { "Miner Mk1",        60, 1, true  },
        /* MinerMk2       */ { "Miner Mk2",       120, 1, true  },
        /* MinerMk3       */ { "Miner Mk3",       240, 1, true  },
        /* WaterExtractor */ { "Water Extractor", 120, 1, false },
        /* OilExtractor   */ { "Oil Extractor",   120, 1, true  },
    };

    const ExtractorSpec& SpecFor(ExtractorNode::Kind k)
    {
        return kExtractorSpecs[static_cast<int>(k)];
    }

    FractionalNumber PurityMultiplier(ExtractorNode::Purity p)
    {
        switch (p)
        {
        case ExtractorNode::Purity::Impure: return FractionalNumber(1, 2);
        case ExtractorNode::Purity::Pure:   return FractionalNumber(2, 1);
        case ExtractorNode::Purity::Normal:
        default:                            return FractionalNumber(1, 1);
        }
    }
}

ExtractorNode::ExtractorNode(const ax::NodeEditor::NodeId id, ExtractorNode::Kind extractor_kind,
    const Item* resource, ExtractorNode::Purity purity,
    const std::function<unsigned long long int()>& id_generator) :
    PoweredNode(id), extractor_kind(extractor_kind), resource(resource), purity(purity)
{
    // Single output pin carrying the extracted resource at the (per-kind) base
    // rate; UpdateRate then scales by purity × current_rate.
    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, resource, false, GetBaseRate()));
    UpdateRate(current_rate);
}

ExtractorNode::ExtractorNode(const ax::NodeEditor::NodeId id, const std::function<unsigned long long int()>& id_generator, const Json::Value& serialized, const INodeDataResolver& resolver) :
    PoweredNode(id, serialized)
{
    if (static_cast<Node::Kind>(serialized["kind"].get<int>()) != Node::Kind::Extractor)
    {
        throw std::runtime_error("Trying to deserialize an unvalid node as an extractor node");
    }

    extractor_kind = static_cast<ExtractorNode::Kind>(serialized["extractor_kind"].get<int>());
    purity = static_cast<ExtractorNode::Purity>(serialized["purity"].get<int>());

    resource = nullptr;
    const std::string& resource_name = serialized["resource"].get_string();
    if (!resource_name.empty())
    {
        resource = resolver.FindItem(resource_name);
        if (resource == nullptr)
        {
            throw std::runtime_error("Unknown item when loading extractor node");
        }
    }

    outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, resource, false, GetBaseRate()));
    UpdateRate(current_rate);
    const bool locked = serialized["locked"].get<bool>();
    for (auto& p : outs)
    {
        p->SetLocked(locked);
    }
}

ExtractorNode::~ExtractorNode()
{

}

Node::Kind ExtractorNode::GetKind() const
{
    return Node::Kind::Extractor;
}

bool ExtractorNode::IsExtractor() const
{
    return true;
}

Json::Value ExtractorNode::Serialize() const
{
    Json::Value node = PoweredNode::Serialize();
    node["extractor_kind"] = static_cast<int>(extractor_kind);
    node["resource"] = resource == nullptr ? "" : resource->name;
    node["purity"] = static_cast<int>(purity);
    return node;
}

void ExtractorNode::UpdateRate(const FractionalNumber& new_rate)
{
    current_rate = new_rate;
    // No inputs to update. Output rate = base × purity × clock (current_rate).
    // We compute base_rate live (from extractor_kind) instead of reading
    // Pin::base_rate, because Pin::base_rate is const and would otherwise
    // require destroying + recreating the pin whenever the Mk level changes —
    // which would dangle any Link pointing at this pin.
    const FractionalNumber out_rate = GetBaseRate() * GetPurityMultiplier() * current_rate;
    for (auto& p : outs)
    {
        p->current_rate = out_rate;
    }
    ComputePowerUsage();
}

bool ExtractorNode::HasVariablePower() const
{
    const Building* b = GetBuilding();
    return b != nullptr && b->variable_power;
}

void ExtractorNode::ComputePowerUsage()
{
    const Building* building = GetBuilding();
    if (building == nullptr)
    {
        same_clock_power = FractionalNumber(0, 1);
        last_underclock_power = FractionalNumber(0, 1);
        return;
    }
    // Mirrors CraftNode::ComputePowerUsage but without the somersloop branch
    // (extractors don't accept production amplifiers in Satisfactory 1.0).
    const int num_machines = static_cast<int>(std::ceil(current_rate.GetValue()));
    const double power = building->power;
    const double same_clock_power_double =
        num_machines *
        power *
        std::pow(current_rate.GetValue() / static_cast<double>(std::max(1, num_machines)), building->power_exponent);
    const int num_full_machines = static_cast<int>(std::floor(current_rate.GetValue()));
    double last_underclock_power_double = num_full_machines * power;
    last_underclock_power_double +=
        power *
        std::pow(current_rate.GetValue() - num_full_machines, building->power_exponent);
    same_clock_power = FractionalNumber(static_cast<long long int>(std::round(same_clock_power_double * 1000.0)), 1000);
    last_underclock_power = FractionalNumber(static_cast<long long int>(std::round(last_underclock_power_double * 1000.0)), 1000);
}

void ExtractorNode::ChangeKind(ExtractorNode::Kind k)
{
    extractor_kind = k;
    // Water Extractor has no purity dimension; pin its purity to Normal so
    // UI/serialization don't carry stale state from a prior solid-miner kind.
    if (!SpecFor(k).supports_purity)
    {
        purity = ExtractorNode::Purity::Normal;
    }
    // UpdateRate reads GetBaseRate() live, so no pin rebuild needed.
    UpdateRate(current_rate);
}

void ExtractorNode::ChangeResource(const Item* new_resource, const std::function<unsigned long long int()>& id_generator)
{
    resource = new_resource;
    if (outs.empty())
    {
        outs.emplace_back(std::make_unique<Pin>(id_generator(), ax::NodeEditor::PinKind::Output, this, resource, false, GetBaseRate()));
    }
    else
    {
        // Pin::item is a non-const pointer to const Item, so reassigning is
        // allowed. Avoids destroying the Pin (which would dangle any Link).
        outs[0]->item = new_resource;
    }
    UpdateRate(current_rate);
}

void ExtractorNode::ChangePurity(ExtractorNode::Purity p)
{
    if (!SpecFor(extractor_kind).supports_purity)
    {
        return; // Water Extractor: silently ignore.
    }
    purity = p;
    UpdateRate(current_rate);
}

const Building* ExtractorNode::GetBuilding() const
{
    const auto it = Data::Buildings().find(SpecFor(extractor_kind).building_name);
    return it == Data::Buildings().end() ? nullptr : it->second.get();
}

FractionalNumber ExtractorNode::GetBaseRate() const
{
    const ExtractorSpec& s = SpecFor(extractor_kind);
    return FractionalNumber(s.base_rate_num, s.base_rate_den);
}

FractionalNumber ExtractorNode::GetPurityMultiplier() const
{
    if (!SpecFor(extractor_kind).supports_purity)
    {
        return FractionalNumber(1, 1);
    }
    return PurityMultiplier(purity);
}

bool ExtractorNode::SupportsPurity() const
{
    return SpecFor(extractor_kind).supports_purity;
}
