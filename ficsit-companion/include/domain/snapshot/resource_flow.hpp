#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "domain/core/fractional_number.hpp"

struct Item;
struct Node;

/// @brief Per-item balance classification.
enum class ResourceFlowStatus { Deficit, Balanced, Surplus };

/// @brief One item's gross produced/consumed totals across the whole graph.
struct ResourceFlowRow
{
    const Item* item = nullptr;
    FractionalNumber produced;          // defaults to 0/1
    FractionalNumber consumed;          // defaults to 0/1
    FractionalNumber net;               // produced - consumed
    ResourceFlowStatus status = ResourceFlowStatus::Balanced;
};

/// @brief Full report: rows (sorted by item name) plus tallies.
struct ResourceFlowReport
{
    std::vector<ResourceFlowRow> rows;
    std::size_t skipped_null_item_pins = 0;
    std::size_t surplus_count = 0;
    std::size_t deficit_count = 0;
    std::size_t balanced_count = 0;
};

/// @brief Scan the in-memory graph and build the per-item resource report.
/// Craft inputs/outputs, extractor outputs and sink inputs are tallied;
/// groups recurse into their subnodes (gross); organizers and logistics
/// contribute nothing. Pins with a null item are skipped and counted.
/// (Extractor/sink/group branches are added in subsequent tasks.)
ResourceFlowReport BuildResourceFlowReport(const std::vector<std::unique_ptr<Node>>& nodes);

/// @brief Table filter mode.
enum class ResourceFlowFilter { All, Deficit, Surplus };

/// @brief True if a row should be shown under the given status filter and
/// case-insensitive name search (empty search matches all).
bool RowPassesFilter(const ResourceFlowRow& row, ResourceFlowFilter filter, std::string_view search);
