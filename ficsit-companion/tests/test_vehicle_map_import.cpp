#include <catch2/catch_test_macros.hpp>

#include "domain/vehicle/vehicle_map.hpp"

#include <string>
#include <vector>

namespace
{
    const char* CompleteLogisticsFixture()
    {
        return R"({
            "logistics": {
                "stations": [
                    {
                        "id": "station-train",
                        "name": "Central",
                        "guid": "guid-central",
                        "kind": "train",
                        "network_id": 11,
                        "pos": [10, -5],
                        "items": ["Iron Ore", "copper INGOT", "iron ore", 42]
                    },
                    {
                        "id": "station-truck",
                        "name": "Outpost",
                        "guid": "guid-outpost",
                        "kind": "truck",
                        "network_id": 22,
                        "pos": [-20, 15],
                        "items": ["Copper Ingot", "Coal"]
                    },
                    {
                        "id": "station-orphan",
                        "name": "Unused",
                        "guid": "guid-unused",
                        "pos": [5, 6],
                        "items": []
                    },
                    {"name": "missing id is ignored", "guid": "ignored"}
                ],
                "vehicles": [
                    {
                        "id": "vehicle-truck",
                        "name": "Hauler",
                        "type": "truck",
                        "network_id": 22,
                        "pos": [30, 40],
                        "autopilot": true,
                        "fuel": "Coal",
                        "route_guids": [
                            "guid-central",
                            "guid-outpost",
                            "guid-central",
                            "unknown-guid"
                        ]
                    },
                    {
                        "id": "vehicle-train",
                        "name": "Express",
                        "type": "train",
                        "pos": [50, 60],
                        "route_guids": ["guid-outpost"]
                    },
                    {
                        "id": "vehicle-orphan",
                        "name": "Lost",
                        "type": "explorer",
                        "pos": [70, 80],
                        "route_guids": ["unknown-guid"]
                    },
                    {"name": "missing id is ignored", "type": "tractor"}
                ],
                "paths": [
                    {
                        "id": "road-1",
                        "kind": "road",
                        "network_id": 22,
                        "waypoints": [[-100, -200], [0, 0], [25, 50]]
                    },
                    {
                        "id": "rail-1",
                        "kind": "rail",
                        "network_id": 11,
                        "waypoints": [[10, 20], [300, 400]]
                    },
                    {"id": "too-short", "kind": "road", "waypoints": [[1, 2]]}
                ]
            }
        })";
    }
}

TEST_CASE("ParseLogisticsJson builds a connected searchable logistics model",
          "[vehicle_map][import]")
{
    const VehicleMap::Model model = VehicleMap::ParseLogisticsJson(CompleteLogisticsFixture());

    REQUIRE(model.ok);
    REQUIRE(model.error.empty());
    REQUIRE_FALSE(model.Empty());

    REQUIRE(model.stations.size() == 3);
    REQUIRE(model.vehicles.size() == 3);
    REQUIRE(model.segments.size() == 2);

    REQUIRE(model.station_index.at("station-train") == 0);
    REQUIRE(model.station_index.at("station-truck") == 1);
    REQUIRE(model.guid_to_station.at("guid-central") == 0);
    REQUIRE(model.guid_to_station.at("guid-outpost") == 1);
    REQUIRE(model.vehicle_index.at("vehicle-truck") == 0);
    REQUIRE(model.vehicle_index.at("vehicle-train") == 1);

    const auto& truck = model.vehicles[0];
    REQUIRE(truck.autopilot);
    REQUIRE(truck.fuel_name == "Coal");
    REQUIRE(truck.station_ids ==
            std::vector<std::string>{"station-train", "station-truck", "station-train"});
    REQUIRE(model.stations[0].vehicle_ids == std::vector<std::string>{"vehicle-truck"});
    REQUIRE(model.stations[1].vehicle_ids ==
            std::vector<std::string>{"vehicle-truck", "vehicle-train"});

    REQUIRE(model.item_names_sorted ==
            std::vector<std::string>{"Coal", "copper INGOT", "Iron Ore"});
    REQUIRE(model.item_index.at("iron ore").stations == std::vector<size_t>{0});
    REQUIRE(model.item_index.at("iron ore").vehicles == std::vector<size_t>{0});
    REQUIRE(model.item_index.at("copper ingot").stations == std::vector<size_t>{0, 1});
    REQUIRE(model.item_index.at("copper ingot").vehicles == std::vector<size_t>{0, 1});

    REQUIRE(model.orphan_stations == std::vector<size_t>{2});
    REQUIRE(model.orphan_vehicles == std::vector<size_t>{2});

    REQUIRE(model.segments[0].kind == VehicleMap::PathKind::Road);
    REQUIRE(model.segments[1].kind == VehicleMap::PathKind::Rail);
    REQUIRE(model.has_bounds);
    REQUIRE(model.world_min.x == -100.0f);
    REQUIRE(model.world_min.y == -200.0f);
    REQUIRE(model.world_max.x == 300.0f);
    REQUIRE(model.world_max.y == 400.0f);
}

/// @test   From an importer's "warnings" array, only the entries mentioning "logistics" are kept;
///         unrelated warnings and non-string entries (e.g. a bare number) are dropped.
/// @covers VehicleMap::ExtractLogisticsWarnings filtering — surfacing just the logistics-relevant
///         import warnings to the user while ignoring noise and non-string array elements.
TEST_CASE("ExtractLogisticsWarnings keeps only logistics lines", "[vehicle_map][import]")
{
    const std::string json = R"({
        "warnings": [
            "logistics: 2 stations missing a network",
            "unrelated belt warning",
            "another logistics note",
            12345
        ]
    })";
    const auto w = VehicleMap::ExtractLogisticsWarnings(json);
    REQUIRE(w.size() == 2);
    REQUIRE(w[0].find("logistics") != std::string::npos);
    REQUIRE(w[1].find("logistics") != std::string::npos);
}

/// @test   The result is empty when there is no usable "warnings" array: a JSON object without the
///         field, and one where "warnings" is a string instead of an array, both yield no warnings.
/// @covers VehicleMap::ExtractLogisticsWarnings schema-mismatch handling (missing key / wrong type),
///         ensuring it never fabricates warnings from malformed-but-valid JSON.
TEST_CASE("ExtractLogisticsWarnings is empty when no warnings field", "[vehicle_map][import]")
{
    REQUIRE(VehicleMap::ExtractLogisticsWarnings(R"({"foo": 1})").empty());
    REQUIRE(VehicleMap::ExtractLogisticsWarnings(R"({"warnings": "not an array"})").empty());
}

/// @test   Unparseable input is handled gracefully: a non-JSON string and an empty string each
///         return an empty list rather than throwing.
/// @covers VehicleMap::ExtractLogisticsWarnings parse-failure robustness on untrusted importer
///         output (the external tool may emit garbage or nothing at all).
TEST_CASE("ExtractLogisticsWarnings tolerates malformed JSON", "[vehicle_map][import]")
{
    REQUIRE(VehicleMap::ExtractLogisticsWarnings("definitely not json {[").empty());
    REQUIRE(VehicleMap::ExtractLogisticsWarnings("").empty());
}

TEST_CASE("ParseLogisticsJson maps every vehicle type and station kind",
          "[vehicle_map][import]")
{
    const auto model = VehicleMap::ParseLogisticsJson(R"({
        "logistics": {
            "stations": [
                {"id": "train", "kind": "train"},
                {"id": "truck", "kind": "truck"},
                {"id": "default", "kind": "anything-else"}
            ],
            "vehicles": [
                {"id": "tractor", "type": "tractor"},
                {"id": "explorer", "type": "explorer"},
                {"id": "cyberwagon", "type": "cyberwagon"},
                {"id": "train", "type": "train"},
                {"id": "truck", "type": "truck"},
                {"id": "unknown", "type": "factory-cart"}
            ]
        }
    })");

    REQUIRE(model.ok);
    REQUIRE(model.stations.size() == 3);
    REQUIRE(model.stations[0].kind == VehicleMap::StationKind::Train);
    REQUIRE(model.stations[1].kind == VehicleMap::StationKind::Truck);
    REQUIRE(model.stations[2].kind == VehicleMap::StationKind::Truck);

    const std::vector<VehicleMap::VehicleType> expected{
        VehicleMap::VehicleType::Tractor,
        VehicleMap::VehicleType::Explorer,
        VehicleMap::VehicleType::CyberWagon,
        VehicleMap::VehicleType::Train,
        VehicleMap::VehicleType::Truck,
        VehicleMap::VehicleType::Unknown,
    };
    REQUIRE(model.vehicles.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        REQUIRE(model.vehicles[i].type == expected[i]);
    }
}

TEST_CASE("VehicleTypeName labels every vehicle enum", "[vehicle_map][import]")
{
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::Truck)) == "Truck");
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::Tractor)) == "Tractor");
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::Explorer)) == "Explorer");
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::CyberWagon)) ==
            "Cyber Wagon");
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::Train)) == "Train");
    REQUIRE(std::string(VehicleMap::VehicleTypeName(VehicleMap::VehicleType::Unknown)) == "Vehicle");
}

TEST_CASE("ParseLogisticsJson defaults invalid positions and skips invalid waypoints",
          "[vehicle_map][import]")
{
    const auto model = VehicleMap::ParseLogisticsJson(R"({
        "logistics": {
            "stations": [
                {"id": "missing"},
                {"id": "short", "pos": [12]},
                {"id": "invalid", "pos": ["bad", 8]},
                {"id": "not-array", "pos": "bad"}
            ],
            "vehicles": [
                {"id": "vehicle-short", "pos": [-3]},
                {"id": "vehicle-invalid", "pos": [4, false]}
            ],
            "paths": [
                {
                    "id": "mixed",
                    "waypoints": [
                        [-10, -20],
                        "not-an-array",
                        [1],
                        ["bad", 2],
                        [3, "bad"],
                        [30, 40]
                    ]
                },
                {
                    "id": "all-invalid",
                    "kind": "rail",
                    "waypoints": [null, [], [1], ["x", "y"]]
                },
                {"id": "wrong-waypoints-type", "waypoints": "not-an-array"}
            ]
        }
    })");

    REQUIRE(model.ok);
    REQUIRE(model.stations.size() == 4);
    REQUIRE(model.stations[0].pos.x == 0.0f);
    REQUIRE(model.stations[0].pos.y == 0.0f);
    REQUIRE(model.stations[1].pos.x == 12.0f);
    REQUIRE(model.stations[1].pos.y == 0.0f);
    REQUIRE(model.stations[2].pos.x == 0.0f);
    REQUIRE(model.stations[2].pos.y == 8.0f);
    REQUIRE(model.stations[3].pos.x == 0.0f);
    REQUIRE(model.stations[3].pos.y == 0.0f);
    REQUIRE(model.vehicles[0].pos.x == -3.0f);
    REQUIRE(model.vehicles[0].pos.y == 0.0f);
    REQUIRE(model.vehicles[1].pos.x == 4.0f);
    REQUIRE(model.vehicles[1].pos.y == 0.0f);

    REQUIRE(model.segments.size() == 1);
    REQUIRE(model.segments[0].id == "mixed");
    REQUIRE(model.segments[0].kind == VehicleMap::PathKind::Road);
    REQUIRE(model.segments[0].waypoints.size() == 2);
    REQUIRE(model.segments[0].waypoints[0].x == -10.0f);
    REQUIRE(model.segments[0].waypoints[0].y == -20.0f);
    REQUIRE(model.segments[0].waypoints[1].x == 30.0f);
    REQUIRE(model.segments[0].waypoints[1].y == 40.0f);
}

TEST_CASE("ParseLogisticsJson rejects malformed or missing logistics input",
          "[vehicle_map][import]")
{
    const auto malformed = VehicleMap::ParseLogisticsJson("not json {[");
    REQUIRE_FALSE(malformed.ok);
    REQUIRE(malformed.error.find("Failed to parse wrapper JSON:") == 0);

    for (const std::string json : {"null", "[]", R"("string")"})
    {
        const auto model = VehicleMap::ParseLogisticsJson(json);
        REQUIRE_FALSE(model.ok);
        REQUIRE(model.error == "Wrapper JSON has no root object");
    }

    for (const std::string json :
         {R"({})", R"({"logistics": null})", R"({"logistics": []})", R"({"logistics": "bad"})"})
    {
        const auto model = VehicleMap::ParseLogisticsJson(json);
        REQUIRE_FALSE(model.ok);
        REQUIRE(model.error == "Save contains no logistics data (no vehicles/stations found)");
    }
}

TEST_CASE("ParseLogisticsJson accepts an empty logistics model", "[vehicle_map][import]")
{
    const auto model = VehicleMap::ParseLogisticsJson(R"({"logistics": {}})");

    REQUIRE(model.ok);
    REQUIRE(model.error.empty());
    REQUIRE(model.Empty());
    REQUIRE_FALSE(model.has_bounds);
    REQUIRE(model.station_index.empty());
    REQUIRE(model.vehicle_index.empty());
    REQUIRE(model.guid_to_station.empty());
    REQUIRE(model.item_index.empty());
    REQUIRE(model.item_names_sorted.empty());
    REQUIRE(model.orphan_stations.empty());
    REQUIRE(model.orphan_vehicles.empty());
}
