#pragma once

#include <memory>
#include <vector>

#include "domain/fractional_number.hpp"

struct Node;
struct Link;
struct VehicleStationNode;
struct Item;
struct Pin;

namespace VehicleRoute
{
    /// @brief All vehicle stations connected to `origin` through route links
    /// (the connected component), including `origin`. Order is unspecified.
    std::vector<VehicleStationNode*> FindPool(VehicleStationNode* origin);

    /// @brief A pool's cargo pins for a single item, split by route side. Only
    /// emitted when BOTH sides carry the item (permissive balancing).
    struct PoolItemGroup
    {
        const Item* item = nullptr;
        std::vector<Pin*> supply;  ///< Load-mode cargo inputs (excl. fuel inlet)
        std::vector<Pin*> demand;  ///< Unload-mode cargo outputs
    };

    /// @brief Group a pool's cargo pins by item. Returns only groups with BOTH
    /// a supply and a demand pin (permissive). Fuel inlets and null-item pins
    /// are excluded; a station's inactive-side belts are ignored.
    std::vector<PoolItemGroup> GroupPoolByItem(const std::vector<VehicleStationNode*>& pool);

    /// @brief Unify cargo item types across a route pool by filling empty cargo
    /// slots: every loaded item is mirrored onto an empty output of each unloader,
    /// and every unloaded item onto an empty cargo input of each loader. Existing
    /// items and the fuel inlet are never overwritten. Run on route changes so a
    /// freshly connected station inherits the route's items.
    void PropagateCargoItems(const std::vector<VehicleStationNode*>& pool);

    struct ItemBalance
    {
        const Item* item = nullptr;
        FractionalNumber supply{ 0, 1 }; ///< sum of loader cargo-input rates
        FractionalNumber demand{ 0, 1 }; ///< sum of unloader cargo-output rates
    };

    /// @brief Per-cargo-item supply (loaders) vs demand (unloaders) over a pool.
    std::vector<ItemBalance> SummarizePool(const std::vector<VehicleStationNode*>& pool);

    /// @brief Re-solve a route pool, seeding from a pool member's active cargo
    /// pin that already carries a non-zero rate. Returns false only if a solve
    /// ran and was rejected; returns true when the pool carries no rate yet.
    bool ResolveRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                          std::vector<std::unique_ptr<Link>>& links,
                          const std::vector<VehicleStationNode*>& pool,
                          float& error_time, float error_flow_duration);

    /// @brief Carry cargo item types across a route pool (PropagateCargoItems),
    /// then re-balance it (ResolveRoutePool). Returns false only if the balance
    /// solve was rejected.
    bool SyncRoutePool(std::vector<std::unique_ptr<Node>>& nodes,
                       std::vector<std::unique_ptr<Link>>& links,
                       const std::vector<VehicleStationNode*>& pool,
                       float& error_time, float error_flow_duration);
}
