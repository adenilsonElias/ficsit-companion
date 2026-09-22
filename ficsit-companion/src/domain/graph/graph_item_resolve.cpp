#include "domain/graph/graph_item_resolve.hpp"

#include "domain/graph/link.hpp"
#include "domain/nodes/node.hpp"
#include "domain/graph/pin.hpp"
#include "domain/gamedata/recipe.hpp"

#include <imgui_node_editor.h>

#include <unordered_set>
#include <vector>

bool IsFuelPin(const Pin* pin)
{
    if (pin == nullptr || pin->node == nullptr) return false;
    if (!pin->node->IsLogistics()) return false;
    if (pin->direction != ax::NodeEditor::PinKind::Input) return false;
    if (pin->node->ins.empty()) return false;
    const LogisticsNode* l = static_cast<const LogisticsNode*>(pin->node);
    if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
        l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
    return pin == pin->node->ins.back().get();
}

bool IsVehiclePlug(const Pin* pin)
{
    if (pin == nullptr || pin->node == nullptr || !pin->node->IsLogistics()) return false;
    const LogisticsNode* l = static_cast<const LogisticsNode*>(pin->node);
    if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
        l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
    return static_cast<const VehicleStationNode*>(l)->plug.get() == pin;
}

bool IsActiveCargoPin(const Pin* pin)
{
    if (pin == nullptr || pin->node == nullptr || !pin->node->IsLogistics()) return false;
    const LogisticsNode* l = static_cast<const LogisticsNode*>(pin->node);
    if (l->logistics_kind != LogisticsNode::Kind::TruckStation &&
        l->logistics_kind != LogisticsNode::Kind::TrainStation) return false;
    const VehicleStationNode* v = static_cast<const VehicleStationNode*>(l);
    if (v->mode == VehicleStationNode::Mode::Load)
    {
        return pin->direction == ax::NodeEditor::PinKind::Input && !IsFuelPin(pin);
    }
    return pin->direction == ax::NodeEditor::PinKind::Output;
}

const Item* ResolveItemThroughChain(Pin* input_pin)
{
    if (input_pin == nullptr) return nullptr;
    if (input_pin->item != nullptr) return input_pin->item;
    if (input_pin->node == nullptr ||
        (!input_pin->node->IsOrganizer() && !input_pin->node->IsLogistics())) return nullptr;

    std::unordered_set<Node*> visited;
    std::vector<Node*> queue;
    queue.push_back(input_pin->node);
    while (!queue.empty())
    {
        Node* n = queue.back();
        queue.pop_back();
        if (!visited.insert(n).second) continue;
        if (!n->IsOrganizer() && !n->IsLogistics()) continue;
        // OrganizerNode::ChangeItem propagates the item to every pin. Logistics
        // nodes keep item data per pin, but checking all pins still gives a
        // useful item hint for manual Miner -> Storage -> Machine chains.
        for (const auto& p : n->ins) { if (p->item != nullptr) return p->item; }
        for (const auto& p : n->outs) { if (p->item != nullptr) return p->item; }
        // Otherwise, follow each outbound link and check its terminus.
        for (const auto& out_pin : n->outs)
        {
            for (const Link* l : out_pin->links)
            {
                Pin* next = l->end;
                if (next == nullptr) continue;
                if (next->item != nullptr) return next->item;
                if (next->node != nullptr) queue.push_back(next->node);
            }
        }
    }
    return nullptr;
}

const Item* ResolveOrganizerItem(Node* origin)
{
    if (origin == nullptr) return nullptr;
    std::unordered_set<Node*> visited;
    std::vector<Node*> queue;
    queue.push_back(origin);
    while (!queue.empty())
    {
        Node* n = queue.back();
        queue.pop_back();
        if (!visited.insert(n).second) continue;
        auto inspect = [&](Pin* other) -> const Item* {
            if (other == nullptr || other->node == nullptr) return nullptr;
            Node* on = other->node;
            if (on->IsCraft() || on->IsExtractor())
            {
                if (other->item != nullptr) return other->item;
            }
            else if (on->IsOrganizer() || on->IsLogistics())
            {
                queue.push_back(on);
            }
            return nullptr;
        };
        for (const auto& p : n->ins)
        {
            if (IsFuelPin(p.get())) continue; // fuel inlet is not the cargo item
            for (const Link* l : p->links)
            {
                if (const Item* it = inspect(l->start)) return it;
            }
        }
        for (const auto& p : n->outs)
        {
            for (const Link* l : p->links)
            {
                if (const Item* it = inspect(l->end)) return it;
            }
        }
    }
    return nullptr;
}

void RecalculateOrganizerItemChain(OrganizerNode* origin)
{
    if (origin == nullptr) return;
    const Item* item = ResolveOrganizerItem(origin);
    if (item == nullptr) return;

    std::unordered_set<Node*> visited;
    std::vector<Node*> queue;
    queue.push_back(origin);
    while (!queue.empty())
    {
        Node* n = queue.back();
        queue.pop_back();
        if (!visited.insert(n).second) continue;

        if (n->IsCustomSplitter()) continue; // wall: don't relabel or cross
        if (n->IsOrganizer())
        {
            static_cast<OrganizerNode*>(n)->ChangeItem(item);
        }
        else if (n->IsLogistics())
        {
            // Skip the dedicated fuel inlet: it is isolated from the cargo
            // flow and must keep its own (fuel) item.
            for (auto& p : n->ins) { if (!IsFuelPin(p.get())) p->item = item; }
            for (auto& p : n->outs) p->item = item;
        }

        auto hop = [&](Pin* other) {
            if (other == nullptr || other->node == nullptr) return;
            Node* on = other->node;
            if ((on->IsOrganizer() && !on->IsCustomSplitter()) || on->IsLogistics())
            {
                queue.push_back(on);
            }
        };
        // Don't cross the fuel inlet's link - the fuel supply chain is separate
        // from the cargo chain this flood is unifying.
        for (const auto& p : n->ins) { if (!IsFuelPin(p.get())) { for (const Link* l : p->links) hop(l->start); } }
        for (const auto& p : n->outs) { for (const Link* l : p->links) hop(l->end); }
    }
}

void PropagateExtractorResourceUpstream(Node* origin, const Item* item,
    const std::function<unsigned long long int()>& id_generator)
{
    if (origin == nullptr || item == nullptr) return;
    std::unordered_set<Node*> visited;
    std::vector<Node*> queue;
    queue.push_back(origin);
    while (!queue.empty())
    {
        Node* n = queue.back();
        queue.pop_back();
        if (!visited.insert(n).second) continue;
        if (!n->IsOrganizer() && !n->IsLogistics()) continue;
        for (const auto& in_pin : n->ins)
        {
            for (const Link* l : in_pin->links)
            {
                if (l->start == nullptr) continue;
                Node* prev = l->start->node;
                if (prev == nullptr) continue;
                if (prev->IsExtractor())
                {
                    // Same policy as the direct-link path: only fill in a missing
                    // resource, never overwrite one the user already chose.
                    ExtractorNode* ex = static_cast<ExtractorNode*>(prev);
                    if (ex->resource == nullptr)
                    {
                        ex->ChangeResource(item, id_generator);
                    }
                }
                else if (prev->IsOrganizer() || prev->IsLogistics())
                {
                    queue.push_back(prev);
                }
            }
        }
    }
}
