#pragma once

#include "domain/core/fractional_number.hpp"

#include <imgui_node_editor.h>

#include <vector>

struct Node;
struct Item;
struct Link;
struct CountedItem;

struct Pin
{
    Pin(const ax::NodeEditor::PinId id, const ax::NodeEditor::PinKind direction,
        Node* node, const Item* item, const bool locked = false, const FractionalNumber& base_rate = FractionalNumber(0,1));
    ~Pin();
    void SetLocked(const bool b);
    bool GetLocked() const;

    /// @brief This pin's only link, or nullptr if it has none - or more than one.
    ///        A pin with fan-out has no single "the" link: code that must handle
    ///        fan-out iterates `links` instead of calling this.
    Link* SoleLink() const { return links.size() == 1 ? links.front() : nullptr; }

    const ax::NodeEditor::PinId id;
    const ax::NodeEditor::PinKind direction;
    Node* node;
    const Item* item;
    const FractionalNumber base_rate;
    // A pin can carry several links in the Production Planner (fan-out / fan-in); the pin's
    // rate is the sum of its links' rates. Factory Snapshot, Vehicle Map and the .sav importer
    // never create more than one.
    std::vector<Link*> links;

    FractionalNumber current_rate;
    bool error;
private:
    bool locked;
};
