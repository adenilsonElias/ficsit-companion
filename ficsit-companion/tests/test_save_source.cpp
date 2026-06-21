#include <catch2/catch_test_macros.hpp>

#include "infra/save_source.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

/// @test Every persisted field of SaveSource survives Serialize -> Deserialize.
/// @covers SaveSource::Serialize/Deserialize for the full field set.
TEST_CASE("SaveSource round-trips every field", "[save_source]")
{
    SaveSource in;
    in.node_executable_path = "C:/node/node.exe";
    in.sav_watch_dir = "D:/saves";
    in.sav_watch_world = "Refinery";
    in.sav_watch_enabled = true;
    in.last_sav_path = "D:/saves/Refinery_autosave_1.sav";
    in.layout_mode = SavImport::LayoutMode::World;
    in.world_spacing_scale = 0.123f;
    in.connect_vehicle_routes = true;

    SaveSource out;
    out.Deserialize(in.Serialize());

    REQUIRE(out.node_executable_path == in.node_executable_path);
    REQUIRE(out.sav_watch_dir == in.sav_watch_dir);
    REQUIRE(out.sav_watch_world == in.sav_watch_world);
    REQUIRE(out.sav_watch_enabled == in.sav_watch_enabled);
    REQUIRE(out.last_sav_path == in.last_sav_path);
    REQUIRE(out.layout_mode == in.layout_mode);
    REQUIRE(out.world_spacing_scale == in.world_spacing_scale);
    REQUIRE(out.connect_vehicle_routes == in.connect_vehicle_routes);
}

/// @test The shared import options map onto a SavImport::BuildOptions so every
///       tool builds the imported graph with identical settings (the modeler's
///       behavior, now driven from the single global load bar).
/// @covers SaveSource::ToBuildOptions.
TEST_CASE("SaveSource exposes its import options as BuildOptions", "[save_source]")
{
    SaveSource s;
    s.layout_mode = SavImport::LayoutMode::World;
    s.world_spacing_scale = 0.2f;
    s.connect_vehicle_routes = true;

    const SavImport::BuildOptions options = s.ToBuildOptions();
    REQUIRE(options.layout_mode == SavImport::LayoutMode::World);
    REQUIRE(options.world_spacing_scale == 0.2f);
    REQUIRE(options.connect_vehicle_routes == true);
}

/// @test Malformed JSON leaves a pre-set field untouched (graceful degradation).
/// @covers SaveSource::Deserialize parse-failure path.
TEST_CASE("SaveSource keeps defaults for malformed JSON", "[save_source]")
{
    SaveSource s;
    s.sav_watch_world = "Seed";
    s.Deserialize("not json {");
    REQUIRE(s.sav_watch_world == "Seed");
}

/// @test Absent keys keep current values (sparse/old-blob compatibility).
/// @covers SaveSource::Deserialize per-key merge semantics.
TEST_CASE("SaveSource keeps current values for absent keys", "[save_source]")
{
    SaveSource s;
    s.sav_watch_dir = "kept";
    s.Deserialize("{\"sav_watch_world\":\"Other\"}");
    REQUIRE(s.sav_watch_world == "Other");
    REQUIRE(s.sav_watch_dir == "kept");
}

/// @test The shared save source can resolve the latest .sav matching the selected
///       world, so the global load bar can import by folder/world just like the
///       older per-tool import controls.
/// @covers SaveSource::FindLatestSav path filtering and timestamp ordering.
TEST_CASE("SaveSource finds latest matching save file", "[save_source]")
{
    namespace fs = std::filesystem;

    const fs::path dir = fs::temp_directory_path() / "fc-save-source-latest-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const fs::path older = dir / "Factory_autosave_0.sav";
    const fs::path newer = dir / "Factory_autosave_1.sav";
    const fs::path other_world = dir / "Other_autosave_9.sav";
    std::ofstream(older.string()) << "old";
    std::ofstream(newer.string()) << "new";
    std::ofstream(other_world.string()) << "other";

    const auto base_time = fs::file_time_type::clock::now();
    fs::last_write_time(older, base_time);
    fs::last_write_time(newer, base_time + std::chrono::seconds(1));
    fs::last_write_time(other_world, base_time + std::chrono::seconds(2));

    SaveSource source;
    source.sav_watch_dir = dir.string();
    source.sav_watch_world = "Factory";

    REQUIRE(source.FindLatestSav() == newer.string());

    fs::remove_all(dir);
}
