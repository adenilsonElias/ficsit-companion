#include <catch2/catch_test_macros.hpp>

#include "infra/persistence/factory_snapshot_session.hpp"

/// @test Every persisted view-pref field survives Serialize -> Deserialize.
/// @covers FactorySnapshotSession::Serialize/Deserialize full field set.
TEST_CASE("FactorySnapshotSession round-trips every field", "[snapshot_session]")
{
    FactorySnapshotSession in;
    in.flow_filter = 2;          // Surplus
    in.flow_search = "water";
    in.world_layout = true;
    in.node_font_scale = 1.75f;
    in.icon_scale = 2.5f;
    in.hide_logistics_enabled = true;
    in.hide_game_splitters = false;
    in.hide_custom_splitters = true;
    in.hide_mergers = false;
    in.hide_logistics_nodes = true;

    FactorySnapshotSession out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.flow_filter == in.flow_filter);
    REQUIRE(out.flow_search == in.flow_search);
    REQUIRE(out.world_layout == in.world_layout);
    REQUIRE(out.node_font_scale == in.node_font_scale);
    REQUIRE(out.icon_scale == in.icon_scale);
    REQUIRE(out.hide_logistics_enabled == in.hide_logistics_enabled);
    REQUIRE(out.hide_game_splitters == in.hide_game_splitters);
    REQUIRE(out.hide_custom_splitters == in.hide_custom_splitters);
    REQUIRE(out.hide_mergers == in.hide_mergers);
    REQUIRE(out.hide_logistics_nodes == in.hide_logistics_nodes);
}

/// @test Out-of-range slider scales are clamped into their allowed bounds.
/// @covers FactorySnapshotSession node_font_scale / icon_scale clamping.
TEST_CASE("FactorySnapshotSession clamps slider scales", "[snapshot_session]")
{
    FactorySnapshotSession lo;
    lo.Deserialize("{\"node_font_scale\": 0.1, \"icon_scale\": 0.0}");
    REQUIRE(lo.node_font_scale == 0.5f);
    REQUIRE(lo.icon_scale == 0.5f);

    FactorySnapshotSession hi;
    hi.Deserialize("{\"node_font_scale\": 99.0, \"icon_scale\": 99.0}");
    REQUIRE(hi.node_font_scale == 3.0f);
    REQUIRE(hi.icon_scale == 4.0f);
}

/// @test Malformed JSON leaves a pre-set field untouched.
TEST_CASE("FactorySnapshotSession keeps defaults for malformed JSON", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.flow_search = "kept";
    s.Deserialize("nope {");
    REQUIRE(s.flow_search == "kept");
}

/// @test An out-of-range flow_filter is clamped to 0 (All).
TEST_CASE("FactorySnapshotSession clamps invalid flow_filter", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"flow_filter\": 99}");
    REQUIRE(s.flow_filter == 0);
}

/// @test A session JSON without the hide-* keys leaves their defaults intact.
TEST_CASE("FactorySnapshotSession keeps hide defaults when keys absent", "[snapshot_session]")
{
    FactorySnapshotSession s;
    s.Deserialize("{\"flow_filter\": 1}");
    REQUIRE(s.hide_logistics_enabled == false);
    REQUIRE(s.hide_game_splitters == true);
    REQUIRE(s.hide_custom_splitters == true);
    REQUIRE(s.hide_mergers == true);
    REQUIRE(s.hide_logistics_nodes == true);
}
