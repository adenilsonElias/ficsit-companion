#include <catch2/catch_test_macros.hpp>
#include "infra/file_store.hpp"
#include "infra/settings_store.hpp"

/// @test   Every scalar setting survives a Save → Load round-trip through the store: bools
///         (show_somersloop, power_equal_clocks, left_panel_folded), strings (sav_watch_dir,
///         sav_watch_world), and a float (sav_import_world_spacing_scale) all read back exactly,
///         including non-default values.
/// @covers SettingsStore::Save/Load against an InMemoryFileStore — JSON key mapping and type
///         fidelity for the full Settings struct. Catches dropped or mistyped persisted fields.
TEST_CASE("SettingsStore round-trips scalar settings", "[settings]")
{
    InMemoryFileStore fs;
    SettingsStore store(fs, "settings.json");
    Settings s;
    s.show_somersloop = true;
    s.power_equal_clocks = false;
    s.left_panel_folded = true;
    s.sav_watch_dir = "C:/saves";
    s.sav_watch_world = "MyWorld";
    s.sav_import_world_spacing_scale = 2.5f;
    s.show_resource_flow = true;
    s.show_debug_ids = true;
    store.Save(s);
    Settings loaded;
    store.Load(loaded, {});
    REQUIRE(loaded.show_somersloop == true);
    REQUIRE(loaded.power_equal_clocks == false);
    REQUIRE(loaded.left_panel_folded == true);
    REQUIRE(loaded.sav_watch_dir == "C:/saves");
    REQUIRE(loaded.sav_watch_world == "MyWorld");
    REQUIRE(loaded.sav_import_world_spacing_scale == 2.5f);
    REQUIRE(loaded.show_resource_flow == true);
    REQUIRE(loaded.show_debug_ids == true);
}

/// @test   Loading when no settings file exists falls back to the documented defaults rather than
///         leaving the struct uninitialized: power_equal_clocks defaults to false and
///         sav_import_world_spacing_scale defaults to SavImport::kPositionScale.
/// @covers SettingsStore::Load's missing-file / absent-key default path, locking in the same
///         defaults the original LoadSettings produced for a first-run user.
TEST_CASE("SettingsStore Load applies defaults when file absent", "[settings]")
{
    InMemoryFileStore fs;
    SettingsStore store(fs, "settings.json");
    Settings loaded;
    store.Load(loaded, {});
    REQUIRE(loaded.power_equal_clocks == false); // JSON-absent default per original LoadSettings
    REQUIRE(loaded.sav_import_world_spacing_scale == SavImport::kPositionScale);
    REQUIRE(loaded.show_debug_ids == false);
}
