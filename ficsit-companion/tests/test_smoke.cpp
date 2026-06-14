#include <catch2/catch_test_macros.hpp>

/// @test   The test harness itself is healthy: Catch2's main is linked and a trivial assertion runs.
/// @covers No production code — only build/link wiring of fc-tests. A green run proves the suite
///         compiles, links, and executes before any real test is trusted.
TEST_CASE("smoke: test harness links and runs", "[smoke]")
{
    REQUIRE(1 + 1 == 2);
}
