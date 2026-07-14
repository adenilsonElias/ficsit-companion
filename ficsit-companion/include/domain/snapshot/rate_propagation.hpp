#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

struct Node;
struct Link;

/// @brief Forward-propagate item rates from producer outputs through the wired
/// graph to a fixed point.
///
/// Producers (CraftNode / ExtractorNode) must already have their output pin
/// `current_rate` set (from recipe x clock x somersloop). This pass then fills
/// the rates of the flow/logistics graph in between:
///   - Merger output   = sum(inputs)
///   - Splitter outputs = input / count(connected outputs)
///   - Logistics outputs = sum(non-fuel inputs) / count(connected outputs)
///   - Each link copies the upstream output rate onto the downstream input pin,
///     EXCEPT CraftNode/Extractor input pins, whose rates are recipe-fixed and
///     must never be overwritten.
///
/// Items pass through logistics/organizer nodes as well (a storage/station
/// carries whatever its inputs carry), except for nodes listed in
/// @p mixed_item_nodes, which physically mix different cargo and are left as-is.
///
/// Pure; no UI. Mutates pin `current_rate` (and logistics pin `item`) only;
/// graph topology is never changed. Iterates to a fixed point so multi-hop
/// chains converge.
void PropagateRates(const std::vector<std::unique_ptr<Node>>& nodes,
                    const std::vector<std::unique_ptr<Link>>& links,
                    const std::unordered_set<const Node*>& mixed_item_nodes = {});
