#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "infra/persistence/vehicle_map_session.hpp"
#include "domain/vehicle/vehicle_map_camera.hpp"

using Catch::Approx;

/// @test   Every persisted field of a VehicleMapSession survives Serialize → Deserialize: paths,
///         watch flags, camera pan/zoom, the station/vehicle/item selection, search text, the
///         vehicle type mask, all five layer toggles, and color_by_item.
/// @covers VehicleMapSession::Serialize/Deserialize for the complete field set. A field added to
///         the struct but forgotten in (de)serialization will surface as a mismatch here.
TEST_CASE("VehicleMapSession round-trips every field", "[vehicle_map][session]")
{
    VehicleMapSession in;
    in.node_executable_path = "C:/node/node.exe";
    in.sav_watch_dir = "D:/saves";
    in.sav_watch_world = "Refinery";
    in.sav_watch_enabled = true;
    in.last_sav_path = "D:/saves/Refinery_autosave_1.sav";
    in.pan = ImVec2(-128.5f, 64.25f);
    in.zoom = 0.137f;
    in.sel_station = "s7";
    in.sel_vehicle = "v3";
    in.sel_item = "Iron Ore";
    in.search = "copper";
    in.type_mask = 0x2A;
    in.layers.roads = false;
    in.layers.rails = false;
    in.layers.vehicles = true;
    in.layers.stations = false;
    in.layers.labels = true;
    in.color_by_item = true;

    VehicleMapSession out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.node_executable_path == in.node_executable_path);
    REQUIRE(out.sav_watch_dir == in.sav_watch_dir);
    REQUIRE(out.sav_watch_world == in.sav_watch_world);
    REQUIRE(out.sav_watch_enabled == in.sav_watch_enabled);
    REQUIRE(out.last_sav_path == in.last_sav_path);
    REQUIRE(out.pan.x == Approx(in.pan.x));
    REQUIRE(out.pan.y == Approx(in.pan.y));
    REQUIRE(out.zoom == Approx(in.zoom));
    REQUIRE(out.sel_station == in.sel_station);
    REQUIRE(out.sel_vehicle == in.sel_vehicle);
    REQUIRE(out.sel_item == in.sel_item);
    REQUIRE(out.search == in.search);
    REQUIRE(out.type_mask == in.type_mask);
    REQUIRE(out.layers.roads == in.layers.roads);
    REQUIRE(out.layers.rails == in.layers.rails);
    REQUIRE(out.layers.vehicles == in.layers.vehicles);
    REQUIRE(out.layers.stations == in.layers.stations);
    REQUIRE(out.layers.labels == in.layers.labels);
    REQUIRE(out.color_by_item == in.color_by_item);
}

/// @test   Deserializing malformed JSON leaves a pre-set field untouched and keeps zoom at its
///         default — a corrupt persisted blob must not wipe in-memory state.
/// @covers VehicleMapSession::Deserialize parse-failure path (graceful degradation rather than
///         throwing or zeroing the session).
TEST_CASE("VehicleMapSession keeps defaults for malformed JSON", "[vehicle_map][session]")
{
    VehicleMapSession s;
    s.sav_watch_world = "Seed";
    s.Deserialize("this is not json {");
    REQUIRE(s.sav_watch_world == "Seed"); // untouched
    REQUIRE(s.zoom == Approx(VehicleMapCamera::kDefaultZoom));
}

/// @test   Deserializing partial JSON applies the keys that are present (sel_item) while leaving
///         fields whose keys are absent (sav_watch_dir, type_mask) at their current values.
/// @covers VehicleMapSession::Deserialize per-key merge semantics — forward/backward compatibility
///         with sparse or older saved blobs that omit some keys.
TEST_CASE("VehicleMapSession keeps current values for absent keys", "[vehicle_map][session]")
{
    VehicleMapSession s;
    s.sav_watch_dir = "kept";
    s.type_mask = 0x99;
    s.Deserialize("{\"sel_item\":\"Plastic\"}");
    REQUIRE(s.sel_item == "Plastic"); // present key applied
    REQUIRE(s.sav_watch_dir == "kept"); // absent key retained
    REQUIRE(s.type_mask == 0x99);       // absent key retained
}

/// @test   A persisted non-positive zoom is rejected and replaced by the default: both a negative
///         (-1.0) and a zero zoom deserialize to VehicleMapCamera::kDefaultZoom.
/// @covers VehicleMapSession::Deserialize zoom-sanitization guard, preventing an invalid saved zoom
///         from producing a degenerate (blank/inverted) camera on load.
TEST_CASE("VehicleMapSession clamps non-positive zoom to default", "[vehicle_map][session]")
{
    VehicleMapSession s;
    s.Deserialize("{\"zoom\": -1.0}");
    REQUIRE(s.zoom == Approx(VehicleMapCamera::kDefaultZoom));

    VehicleMapSession z;
    z.Deserialize("{\"zoom\": 0.0}");
    REQUIRE(z.zoom == Approx(VehicleMapCamera::kDefaultZoom));
}
