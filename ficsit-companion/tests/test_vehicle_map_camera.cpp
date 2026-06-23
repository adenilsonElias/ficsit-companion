#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "domain/vehicle/vehicle_map_camera.hpp"

using Catch::Approx;

/// @test   Screen↔world projection is a true inverse: for several world points, under a non-trivial
///         canvas offset, pan, and zoom, ScreenToWorld(WorldToScreen(p)) returns p (within 1e-3).
/// @covers VehicleMapCamera::WorldToScreen / ScreenToWorld round-trip math. Guards against
///         transform drift that would make clicks land on the wrong map coordinate.
TEST_CASE("VehicleMapCamera ScreenToWorld inverts WorldToScreen", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(10.0f, 20.0f), ImVec2(800.0f, 600.0f));
    cam.SetPan(ImVec2(-37.5f, 64.0f));
    cam.SetZoom(0.13f);

    for (const ImVec2 w : { ImVec2(0.0f, 0.0f), ImVec2(1234.0f, -987.0f), ImVec2(-50.0f, 50.0f) })
    {
        const ImVec2 round = cam.ScreenToWorld(cam.WorldToScreen(w));
        REQUIRE(round.x == Approx(w.x).margin(1e-3));
        REQUIRE(round.y == Approx(w.y).margin(1e-3));
    }
}

/// @test   Zooming about a screen anchor scales zoom by the expected factor (×1.1 per notch) while
///         the world point under the anchor stays put on screen before and after the zoom.
/// @covers VehicleMapCamera::ZoomAbout — the "zoom toward the cursor" behavior: it must adjust pan
///         to compensate so the anchored point is invariant, not just multiply the zoom level.
TEST_CASE("VehicleMapCamera ZoomAbout keeps the anchored world point fixed", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(0.0f, 0.0f), ImVec2(640.0f, 480.0f));
    cam.SetPan(ImVec2(100.0f, -20.0f));
    cam.SetZoom(0.05f);

    const ImVec2 anchor(320.0f, 240.0f);
    const ImVec2 world_before = cam.ScreenToWorld(anchor);
    cam.ZoomAbout(anchor, 1.0f); // one wheel notch in

    REQUIRE(cam.Zoom() == Approx(0.05f * 1.1f));
    const ImVec2 world_after = cam.ScreenToWorld(anchor);
    REQUIRE(world_after.x == Approx(world_before.x).margin(1e-2));
    REQUIRE(world_after.y == Approx(world_before.y).margin(1e-2));
}

/// @test   Repeated zooming never escapes the allowed range: 50 zoom-ins stay ≤ 5.0 and 200
///         zoom-outs stay ≥ 1e-4, at both ends of the scale.
/// @covers VehicleMapCamera::ZoomAbout clamping at the upper and lower zoom bounds, preventing
///         degenerate (inverted or infinitesimal) transforms from runaway wheel input.
TEST_CASE("VehicleMapCamera ZoomAbout clamps to [1e-4, 5]", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f));
    cam.SetZoom(4.9f);
    for (int i = 0; i < 50; ++i) cam.ZoomAbout(ImVec2(50.0f, 50.0f), 1.0f);
    REQUIRE(cam.Zoom() <= 5.0f);
    cam.SetZoom(1e-3f);
    for (int i = 0; i < 200; ++i) cam.ZoomAbout(ImVec2(50.0f, 50.0f), -1.0f);
    REQUIRE(cam.Zoom() >= 1e-4f);
}

/// @test   FitAll frames a world-space bounding box so its center maps to the canvas center
///         (accounting for the canvas origin offset) and leaves a positive zoom.
/// @covers VehicleMapCamera::FitAll centering math — the "fit everything into view" action used on
///         map load / reset.
TEST_CASE("VehicleMapCamera FitAll centers bounds in the canvas", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(15.0f, 25.0f), ImVec2(200.0f, 100.0f));
    cam.FitAll(ImVec2(-100.0f, -50.0f), ImVec2(100.0f, 50.0f), true);

    // The bounds center must land on the canvas center, regardless of zoom.
    const ImVec2 center(0.0f, 0.0f);
    const ImVec2 screen = cam.WorldToScreen(center);
    REQUIRE(screen.x == Approx(15.0f + 200.0f * 0.5f));
    REQUIRE(screen.y == Approx(25.0f + 100.0f * 0.5f));
    REQUIRE(cam.Zoom() > 0.0f);
}

/// @test   FitAll with its has_bounds flag false leaves zoom and pan exactly as they were.
/// @covers VehicleMapCamera::FitAll early-out / guard path — an empty map must not snap the camera
///         to a garbage transform.
TEST_CASE("VehicleMapCamera FitAll is a no-op without bounds", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(0.0f, 0.0f), ImVec2(200.0f, 100.0f));
    cam.SetZoom(0.42f);
    cam.SetPan(ImVec2(7.0f, 8.0f));
    cam.FitAll(ImVec2(0.0f, 0.0f), ImVec2(10.0f, 10.0f), false);
    REQUIRE(cam.Zoom() == Approx(0.42f));
    REQUIRE(cam.Pan().x == Approx(7.0f));
    REQUIRE(cam.Pan().y == Approx(8.0f));
}

/// @test   An animated fly-to completes when its progress saturates (dt large enough): it deactivates,
///         lands on the requested target zoom, and centers the target world point on the canvas.
/// @covers VehicleMapCamera::StartFlyTo / UpdateFlyTo / FlyToActive — the camera animation's end
///         state (target zoom + centered point), independent of the easing curve.
TEST_CASE("VehicleMapCamera fly-to reaches its target and centers the point", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(0.0f, 0.0f), ImVec2(400.0f, 300.0f));
    cam.SetZoom(0.02f);

    const ImVec2 target_world(500.0f, -300.0f);
    cam.StartFlyTo(target_world, 0.2f);
    REQUIRE(cam.FlyToActive());

    cam.UpdateFlyTo(1.0f); // dt large enough that t saturates at 1.0
    REQUIRE_FALSE(cam.FlyToActive());
    REQUIRE(cam.Zoom() == Approx(0.2f));

    // Target world point should sit at the canvas center.
    const ImVec2 screen = cam.WorldToScreen(target_world);
    REQUIRE(screen.x == Approx(200.0f).margin(1e-2));
    REQUIRE(screen.y == Approx(150.0f).margin(1e-2));
}

/// @test   A manual PanBy adds the delta to the current pan and cancels any in-progress fly-to.
/// @covers VehicleMapCamera::PanBy — both the translation and the interaction rule that user
///         dragging interrupts the camera animation (so it doesn't fight the user).
TEST_CASE("VehicleMapCamera PanBy translates and cancels fly-to", "[vehicle_map][camera]")
{
    VehicleMapCamera cam;
    cam.SetCanvas(ImVec2(0.0f, 0.0f), ImVec2(400.0f, 300.0f));
    cam.SetPan(ImVec2(10.0f, 10.0f));
    cam.StartFlyTo(ImVec2(1.0f, 1.0f), 0.1f);
    REQUIRE(cam.FlyToActive());

    cam.PanBy(ImVec2(5.0f, -3.0f));
    REQUIRE(cam.Pan().x == Approx(15.0f));
    REQUIRE(cam.Pan().y == Approx(7.0f));
    REQUIRE_FALSE(cam.FlyToActive());
}
