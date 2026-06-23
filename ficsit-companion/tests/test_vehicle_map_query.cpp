#include <catch2/catch_test_macros.hpp>

#include "domain/vehicle/vehicle_map_query.hpp"

namespace
{
    // Two stations served by one truck; "Iron Ore" indexed to s1 + v1.
    VehicleMap::Model MakeModel()
    {
        VehicleMap::Model m;

        VehicleMap::Station s1;
        s1.id = "s1"; s1.name = "Iron Station"; s1.pos = ImVec2(0.0f, 0.0f);
        s1.item_names = { "Iron Ore" };
        s1.vehicle_ids = { "v1" };

        VehicleMap::Station s2;
        s2.id = "s2"; s2.name = "Copper Hub"; s2.pos = ImVec2(100.0f, 0.0f);
        s2.item_names = { "Copper Ore" };
        s2.vehicle_ids = { "v1" };

        m.stations = { s1, s2 };

        VehicleMap::Vehicle v1;
        v1.id = "v1"; v1.name = "Truck Alpha"; v1.type = VehicleMap::VehicleType::Truck;
        v1.station_ids = { "s1", "s2" };
        m.vehicles = { v1 };

        m.station_index = { { "s1", 0 }, { "s2", 1 } };
        m.vehicle_index = { { "v1", 0 } };
        VehicleMap::ItemRefs iron; iron.stations = { 0 }; iron.vehicles = { 0 };
        m.item_index = { { "iron ore", iron } };
        return m;
    }
}

/// @test   Case-insensitive substring search matches regardless of case ("iron"/"ORE" both hit
///         "Iron Ore"), treats an empty needle as a match-all, and rejects an absent substring.
/// @covers VehicleMapQuery::ContainsCI — the primitive every name/item filter builds on; its
///         empty-needle rule is what makes a blank search box show everything.
TEST_CASE("ContainsCI is case-insensitive and empty-needle matches", "[vehicle_map][query]")
{
    REQUIRE(VehicleMapQuery::ContainsCI("Iron Ore", "iron"));
    REQUIRE(VehicleMapQuery::ContainsCI("Iron Ore", "ORE"));
    REQUIRE(VehicleMapQuery::ContainsCI("anything", ""));
    REQUIRE_FALSE(VehicleMapQuery::ContainsCI("Iron Ore", "copper"));
}

/// @test   A station passes the search filter when the query is empty, matches its name ("iron"),
///         or matches any of its served item names ("ore"), and fails on an unrelated query.
/// @covers VehicleMapQuery::StationPassesFilter — that station filtering searches both the station
///         name and its item_names list, not just the name.
TEST_CASE("StationPassesFilter matches name and item names", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    const VehicleMap::Station& s1 = m.stations[0];

    REQUIRE(VehicleMapQuery::StationPassesFilter(s1, ""));        // empty = match all
    REQUIRE(VehicleMapQuery::StationPassesFilter(s1, "iron"));    // by name
    REQUIRE(VehicleMapQuery::StationPassesFilter(s1, "ore"));     // by item name
    REQUIRE_FALSE(VehicleMapQuery::StationPassesFilter(s1, "copper"));
}

/// @test   A vehicle passes the filter only when both gates agree: its type bit is set in the mask
///         (clearing the Truck bit hides a truck) AND the free-text query matches its name ("alpha")
///         or its type name ("truck"); an unrelated query ("train") fails.
/// @covers VehicleMapQuery::VehiclePassesFilter — the combined type-bitmask + free-text predicate
///         behind the vehicle-type toggles and the search box.
TEST_CASE("VehiclePassesFilter respects type mask and free text", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    const VehicleMap::Vehicle& v1 = m.vehicles[0];
    const unsigned int all = 0xFFFFFFFFu;
    const unsigned int truck_bit = 1u << static_cast<unsigned int>(VehicleMap::VehicleType::Truck);

    REQUIRE(VehicleMapQuery::VehiclePassesFilter(v1, "", all));
    REQUIRE_FALSE(VehicleMapQuery::VehiclePassesFilter(v1, "", all & ~truck_bit)); // type filtered out
    REQUIRE(VehicleMapQuery::VehiclePassesFilter(v1, "alpha", all));   // by name
    REQUIRE(VehicleMapQuery::VehiclePassesFilter(v1, "truck", all));   // by type name
    REQUIRE_FALSE(VehicleMapQuery::VehiclePassesFilter(v1, "train", all));
}

/// @test   Selecting a vehicle produces an active highlight set containing that vehicle, all stations
///         on its route, and one route polyline visiting those stops (2 stations → 2-point route).
/// @covers VehicleMapQuery::ComputeHighlight in the "vehicle selected" mode — fan-out from a vehicle
///         to its stations and the route geometry drawn for it.
TEST_CASE("ComputeHighlight for a selected vehicle lights its stations and route", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    const auto h = VehicleMapQuery::ComputeHighlight(m, "v1", "", "");
    REQUIRE(h.active);
    REQUIRE(h.vehicles.count("v1") == 1);
    REQUIRE(h.stations.count("s1") == 1);
    REQUIRE(h.stations.count("s2") == 1);
    REQUIRE(h.routes.size() == 1);
    REQUIRE(h.routes[0].size() == 2);
}

/// @test   Selecting a station produces an active highlight containing that station, every vehicle
///         that serves it, and the route(s) for those vehicles.
/// @covers VehicleMapQuery::ComputeHighlight in the "station selected" mode — the reverse fan-out
///         (station → vehicles) complementing the vehicle-selected case.
TEST_CASE("ComputeHighlight for a selected station lights its vehicles", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    const auto h = VehicleMapQuery::ComputeHighlight(m, "", "s1", "");
    REQUIRE(h.active);
    REQUIRE(h.stations.count("s1") == 1);
    REQUIRE(h.vehicles.count("v1") == 1);
    REQUIRE(h.routes.size() == 1);
}

/// @test   An item query ("Iron Ore") activates the highlight for the whole network that moves that
///         item — the serving station and vehicle — using a case-folded item lookup.
/// @covers VehicleMapQuery::ComputeHighlight in the "item query" mode, including the lower-cased
///         item_index lookup that lets a display-cased query resolve to the indexed key.
TEST_CASE("ComputeHighlight for an item query lights its network", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    const auto h = VehicleMapQuery::ComputeHighlight(m, "", "", "Iron Ore"); // case-folded lookup
    REQUIRE(h.active);
    REQUIRE(h.stations.count("s1") == 1);
    REQUIRE(h.vehicles.count("v1") == 1);
}

/// @test   Highlight is inactive when there is nothing valid to highlight: no selection at all, an
///         unknown vehicle id ("nope"), or an item nobody carries ("Plutonium").
/// @covers VehicleMapQuery::ComputeHighlight negative/edge cases — the active flag must stay false
///         for empty and unresolved inputs so the UI renders no stray highlight.
TEST_CASE("ComputeHighlight is inactive for empty or unknown selection", "[vehicle_map][query]")
{
    const VehicleMap::Model m = MakeModel();
    REQUIRE_FALSE(VehicleMapQuery::ComputeHighlight(m, "", "", "").active);
    REQUIRE_FALSE(VehicleMapQuery::ComputeHighlight(m, "nope", "", "").active);
    REQUIRE_FALSE(VehicleMapQuery::ComputeHighlight(m, "", "", "Plutonium").active);
}
