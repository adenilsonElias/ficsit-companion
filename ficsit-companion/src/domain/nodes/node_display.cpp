#include "domain/nodes/node_display.hpp"

#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"      // Pin::item
#include "domain/gamedata/recipe.hpp"   // Recipe::display_name, Item::name
#include "domain/gamedata/building.hpp" // Building::name

std::string NodeDisplayName(const Node& node)
{
    switch (node.GetKind())
    {
        case Node::Kind::Craft:
        {
            const CraftNode& c = static_cast<const CraftNode&>(node);
            return c.recipe ? c.recipe->display_name : std::string("Recipe");
        }
        case Node::Kind::Extractor:
        {
            const ExtractorNode& e = static_cast<const ExtractorNode&>(node);
            const Building* b = e.GetBuilding();
            std::string title = b ? b->name : std::string("Extractor");
            std::string detail;
            if (e.resource) detail = e.resource->name;
            if (e.SupportsPurity())
            {
                const char* purity_word =
                    e.purity == ExtractorNode::Purity::Impure ? "Impure" :
                    e.purity == ExtractorNode::Purity::Pure   ? "Pure"   : "Normal";
                if (!detail.empty()) detail += ", ";
                detail += purity_word;
            }
            if (!detail.empty()) title += " (" + detail + ")";
            return title;
        }
        case Node::Kind::Merger:         return "Merger";
        case Node::Kind::CustomSplitter: return "Splitter";
        case Node::Kind::GameSplitter:   return "Splitter";
        case Node::Kind::Sink:           return "AWESOME Sink";
        case Node::Kind::Logistics:
        {
            const LogisticsNode& l = static_cast<const LogisticsNode&>(node);
            return l.GetDisplayName();
        }
        case Node::Kind::Group:
        {
            const GroupNode& g = static_cast<const GroupNode&>(node);
            return g.name.empty() ? std::string("Group") : g.name;
        }
    }
    return "Node";
}

const Item* NodePrimaryItem(const Node& node)
{
    for (const auto& pin : node.outs)
    {
        if (pin && pin->item) return pin->item;
    }
    for (const auto& pin : node.ins)
    {
        if (pin && pin->item) return pin->item;
    }
    return nullptr;
}

SnapshotCategory NodeSnapshotCategory(const Node& node)
{
    switch (node.GetKind())
    {
        case Node::Kind::Craft:
        case Node::Kind::Extractor:
            return SnapshotCategory::Production;
        case Node::Kind::CustomSplitter:
        case Node::Kind::GameSplitter:
        case Node::Kind::Merger:
            return SnapshotCategory::Flow;
        case Node::Kind::Sink:
            return SnapshotCategory::Sink;
        case Node::Kind::Group:
            return SnapshotCategory::Other;
        case Node::Kind::Logistics:
        {
            const LogisticsNode& l = static_cast<const LogisticsNode&>(node);
            switch (l.logistics_kind)
            {
                case LogisticsNode::Kind::TruckStation:
                case LogisticsNode::Kind::TrainStation:
                    return SnapshotCategory::Station;
                case LogisticsNode::Kind::PipeJunction:
                    return SnapshotCategory::Flow;
                case LogisticsNode::Kind::Storage:
                case LogisticsNode::Kind::IndustrialStorage:
                case LogisticsNode::Kind::DimensionalDepot:
                case LogisticsNode::Kind::FluidBuffer:
                case LogisticsNode::Kind::IndustrialFluidBuffer:
                    return SnapshotCategory::Storage;
            }
            return SnapshotCategory::Other; // defensive: unknown future logistics kind
        }
    }
    return SnapshotCategory::Other;
}

std::vector<const Node*> NodesProducingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name)
{
    std::vector<const Node*> producers;
    for (const auto& node : nodes)
    {
        if (!node) continue;
        if (!node->IsCraft() && !node->IsExtractor()) continue;
        for (const auto& pin : node->outs)
        {
            if (pin && pin->item && pin->item->name == item_name)
            {
                producers.push_back(node.get());
                break;
            }
        }
    }
    return producers;
}

std::vector<const Node*> NodesConsumingItem(
    const std::vector<std::unique_ptr<Node>>& nodes, const std::string& item_name)
{
    std::vector<const Node*> consumers;
    for (const auto& node : nodes)
    {
        if (!node) continue;
        if (!node->IsCraft() && !node->IsSink()) continue;
        for (const auto& pin : node->ins)
        {
            if (pin && pin->item && pin->item->name == item_name)
            {
                consumers.push_back(node.get());
                break;
            }
        }
    }
    return consumers;
}

bool NodeProductionHidden(const Node& node, const std::set<std::string>& hidden_items)
{
    if (!node.IsCraft() && !node.IsExtractor()) return false;
    bool has_output_item = false;
    for (const auto& pin : node.outs)
    {
        if (!pin || !pin->item) continue;
        has_output_item = true;
        if (hidden_items.find(pin->item->name) == hidden_items.end())
            return false; // an output item is still visible => keep the node
    }
    return has_output_item; // producer with at least one item, all hidden
}
