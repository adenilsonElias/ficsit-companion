#pragma once

#include <optional>

#include <imgui_node_editor.h>

#include "domain/core/fractional_number.hpp"

struct Pin;

struct Link
{
    /// @brief Create a new Link given two Pin ends
    /// @param id Link id
    /// @param start Starting point, must be a non-null pointer to an Output pin
    /// @param end End point, must be a non-null pointer to an Input pin
    Link(const ax::NodeEditor::LinkId id, Pin* start, Pin* end);
    ~Link();

    const ax::NodeEditor::LinkId id;
    /// @brief Output Pin this Link starts out of
    Pin* start;
    /// @brief Input Pin this Link ends to
    Pin* end;
    const ax::NodeEditor::PinId start_id;
    const ax::NodeEditor::PinId end_id;

    /// @brief The rate flowing along this edge. On a single-link pin it equals the rate of both
    ///        ends; on a pin with fan-out, the links' rates sum to the pin's rate.
    FractionalNumber current_rate;

    std::optional<ax::NodeEditor::FlowDirection> flow;
};
