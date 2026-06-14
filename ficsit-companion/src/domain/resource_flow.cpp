#include "domain/resource_flow.hpp"

#include "domain/node.hpp"
#include "domain/pin.hpp"
#include "domain/recipe.hpp"   // Item definition

#include <cctype>
#include <map>
#include <utility>

namespace
{
    struct Agg
    {
        FractionalNumber produced;
        FractionalNumber consumed;
    };

    // Order items by display name (null-safe), so report rows come out sorted.
    struct ByItemName
    {
        bool operator()(const Item* a, const Item* b) const
        {
            return a != nullptr && b != nullptr && a->name < b->name;
        }
    };

    using Totals = std::map<const Item*, Agg, ByItemName>;

    bool ContainsCI(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty()) return true;
        if (needle.size() > haystack.size()) return false;
        const auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
        {
            bool match = true;
            for (std::size_t j = 0; j < needle.size(); ++j)
            {
                if (lower(haystack[i + j]) != lower(needle[j])) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    }

    void AccumulatePins(const std::vector<std::unique_ptr<Pin>>& pins, bool as_produced,
                        Totals& totals, std::size_t& skipped)
    {
        for (const auto& pin : pins)
        {
            if (pin->item == nullptr)
            {
                ++skipped;
                continue;
            }
            Agg& agg = totals[pin->item];
            if (as_produced)
            {
                agg.produced += pin->current_rate;
            }
            else
            {
                agg.consumed += pin->current_rate;
            }
        }
    }

    void Walk(const std::vector<std::unique_ptr<Node>>& nodes, Totals& totals, std::size_t& skipped)
    {
        for (const auto& node : nodes)
        {
            if (node->IsCraft())
            {
                AccumulatePins(node->ins, false, totals, skipped);
                AccumulatePins(node->outs, true, totals, skipped);
            }
            else if (node->IsExtractor())
            {
                AccumulatePins(node->outs, true, totals, skipped);
            }
            else if (node->IsSink())
            {
                AccumulatePins(node->ins, false, totals, skipped);
            }
            else if (node->IsGroup())
            {
                const GroupNode* group = static_cast<const GroupNode*>(node.get());
                Walk(group->nodes, totals, skipped);
            }
            // Organizer / Logistics contribute nothing.
        }
    }
}

ResourceFlowReport BuildResourceFlowReport(const std::vector<std::unique_ptr<Node>>& nodes)
{
    Totals totals;
    ResourceFlowReport report;

    Walk(nodes, totals, report.skipped_null_item_pins);

    const FractionalNumber zero(0, 1);
    for (const auto& [item, agg] : totals) // ByItemName => sorted by item name
    {
        ResourceFlowRow row;
        row.item = item;
        row.produced = agg.produced;
        row.consumed = agg.consumed;
        row.net = agg.produced - agg.consumed;

        if (row.net > zero)
        {
            row.status = ResourceFlowStatus::Surplus;
            ++report.surplus_count;
        }
        else if (row.net < zero)
        {
            row.status = ResourceFlowStatus::Deficit;
            ++report.deficit_count;
        }
        else
        {
            row.status = ResourceFlowStatus::Balanced;
            ++report.balanced_count;
        }
        report.rows.push_back(std::move(row));
    }

    return report;
}

bool RowPassesFilter(const ResourceFlowRow& row, ResourceFlowFilter filter, std::string_view search)
{
    if (filter == ResourceFlowFilter::Deficit && row.status != ResourceFlowStatus::Deficit) return false;
    if (filter == ResourceFlowFilter::Surplus && row.status != ResourceFlowStatus::Surplus) return false;
    if (row.item == nullptr) return search.empty();
    return ContainsCI(row.item->name, search);
}
