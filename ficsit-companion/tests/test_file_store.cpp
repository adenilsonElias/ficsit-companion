#include <catch2/catch_test_macros.hpp>
#include "infra/ports/file_store.hpp"

/// @test   The in-memory file store behaves like a key/value filesystem: a missing key reads as
///         empty, a saved key reads back the exact bytes written, and a removed key reads as empty
///         again.
/// @covers InMemoryFileStore::Load (absent + present), Save, and Remove. Exercises the IFileStore
///         test double used in place of real disk I/O by SettingsStore and serializer tests.
TEST_CASE("InMemoryFileStore round-trips content", "[file_store]")
{
    InMemoryFileStore store;
    REQUIRE_FALSE(store.Load("a.txt").has_value());
    store.Save("a.txt", "hello");
    auto loaded = store.Load("a.txt");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == "hello");
    store.Remove("a.txt");
    REQUIRE_FALSE(store.Load("a.txt").has_value());
}
