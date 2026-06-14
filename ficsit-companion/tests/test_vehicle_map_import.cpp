#include <catch2/catch_test_macros.hpp>

#include "domain/vehicle_map.hpp"

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
