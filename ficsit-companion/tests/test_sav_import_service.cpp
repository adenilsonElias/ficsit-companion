#include <catch2/catch_test_macros.hpp>

#include "infra/sav_import_service.hpp"

/// @test   A save-file stem is reduced to its bare world name by stripping the standard Satisfactory
///         suffixes: a plain name is untouched, while "_CMP", "_autosave_N", and a trailing
///         timestamp ("_YYYY-MM-DD-HH-MM-SS") are removed.
/// @covers SavImport::DeriveWorldName across each suffix shape. Pins down the parsing the watcher
///         uses to group rotating autosaves/timestamps under one logical world.
TEST_CASE("DeriveWorldName strips standard suffixes", "[sav_import]")
{
    REQUIRE(DeriveWorldName("MyWorld") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_CMP") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_autosave_2") == "MyWorld");
    REQUIRE(DeriveWorldName("MyWorld_2024-01-02-03-04-05") == "MyWorld");
}

/// @test   Several save stems for the same worlds collapse to a de-duplicated, alphabetically sorted
///         list of world names — four mixed "Alpha"/"Beta" stems (plain, _CMP, _autosave_N) yield
///         exactly ["Alpha", "Beta"].
/// @covers SavImport::DiscoverWorldNames composed with DeriveWorldName: de-duplication of multiple
///         saves per world and deterministic ordering of the picker list shown to the user.
TEST_CASE("DiscoverWorldNames de-dupes and sorts", "[sav_import]")
{
    std::vector<std::string> stems = {
        "Beta_CMP", "Alpha_autosave_0", "Alpha", "Beta_autosave_1"
    };

    const auto worlds = DiscoverWorldNames(stems);

    REQUIRE(worlds.size() == 2);
    REQUIRE(worlds[0] == "Alpha");
    REQUIRE(worlds[1] == "Beta");
}
