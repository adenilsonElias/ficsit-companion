#pragma once
#include <memory>
#include <vector>
#include "domain/fractional_number.hpp"

struct Link;
struct Node;
struct Pin;

/// @brief Propagates item-rate updates through the node graph using exact
/// rational arithmetic. Extracted verbatim from ProductionApp::UpdateNodesRate.
class RateSolver
{
public:
    /// @param nodes All graph nodes (per-pin error flags are reset across all).
    /// @param links All graph links (link flow is reset/set during the solve).
    /// @param constraint_pin The pin whose rate the user just changed.
    /// @param constraint_value The new rate for that pin.
    /// @param error_time In/out: reset to 0 at start, set to error_flow_duration
    ///        on a rejected update; the caller (UI) uses it to flash the error.
    /// @param error_flow_duration Value assigned to error_time on error paths
    ///        (the app passes ax::NodeEditor::GetStyle().FlowDuration; tests pass any >0).
    /// @return false if the update is rejected (caller should undo the change).
    /// @throws std::runtime_error on an internal propagation inconsistency.
    static bool Solve(std::vector<std::unique_ptr<Node>>& nodes,
                      std::vector<std::unique_ptr<Link>>& links,
                      const Pin* constraint_pin,
                      const FractionalNumber& constraint_value,
                      float& error_time,
                      float error_flow_duration);
};
