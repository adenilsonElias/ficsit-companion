#pragma once

#include <functional>

struct Item;
struct Node;
struct OrganizerNode;
struct Pin;

bool IsFuelPin(const Pin* pin);
/// @brief True when `pin` is the vehicle plug of a Truck/Train station (held
/// outside ins/outs, multi-link, untyped). False for belt pins and other nodes.
bool IsVehiclePlug(const Pin* pin);
/// @brief True for a vehicle station's active cargo pin: a Load-mode cargo input
/// (never the fuel inlet) or an Unload-mode cargo output. These are the belt pins
/// that carry route cargo and seed pool balancing.
bool IsActiveCargoPin(const Pin* pin);
const Item* ResolveItemThroughChain(Pin* input_pin);
const Item* ResolveOrganizerItem(Node* origin);
void RecalculateOrganizerItemChain(OrganizerNode* origin);
void PropagateExtractorResourceUpstream(Node* origin, const Item* item,
    const std::function<unsigned long long int()>& id_generator);
