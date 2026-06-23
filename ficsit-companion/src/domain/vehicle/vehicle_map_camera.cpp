#include "domain/vehicle/vehicle_map_camera.hpp"

#include <algorithm>
#include <cmath>

ImVec2 VehicleMapCamera::WorldToScreen(const ImVec2& world) const
{
    return ImVec2(canvas_p0.x + world.x * zoom + pan.x,
                  canvas_p0.y + world.y * zoom + pan.y);
}

ImVec2 VehicleMapCamera::ScreenToWorld(const ImVec2& screen) const
{
    return ImVec2((screen.x - canvas_p0.x - pan.x) / zoom,
                  (screen.y - canvas_p0.y - pan.y) / zoom);
}

void VehicleMapCamera::SetCanvas(const ImVec2& p0, const ImVec2& size)
{
    canvas_p0 = p0;
    canvas_sz = size;
}

void VehicleMapCamera::FitAll(const ImVec2& world_min, const ImVec2& world_max, bool has_bounds)
{
    if (!has_bounds || canvas_sz.x <= 1.0f || canvas_sz.y <= 1.0f) return;
    const float world_w = std::max(1.0f, world_max.x - world_min.x);
    const float world_h = std::max(1.0f, world_max.y - world_min.y);
    const float margin = 0.9f;
    zoom = std::min(canvas_sz.x / world_w, canvas_sz.y / world_h) * margin;
    if (!std::isfinite(zoom) || zoom <= 0.0f) zoom = kDefaultZoom;
    const ImVec2 center((world_min.x + world_max.x) * 0.5f,
                        (world_min.y + world_max.y) * 0.5f);
    pan = ImVec2(canvas_sz.x * 0.5f - center.x * zoom,
                 canvas_sz.y * 0.5f - center.y * zoom);
    flyto_active = false;
}

void VehicleMapCamera::PanBy(const ImVec2& delta)
{
    pan.x += delta.x;
    pan.y += delta.y;
    flyto_active = false;
}

void VehicleMapCamera::ZoomAbout(const ImVec2& screen_anchor, float wheel)
{
    const ImVec2 world_before = ScreenToWorld(screen_anchor);
    const float factor = std::pow(1.1f, wheel);
    zoom = std::max(1e-4f, std::min(5.0f, zoom * factor));
    // Keep the world point under the anchor fixed.
    pan.x = screen_anchor.x - canvas_p0.x - world_before.x * zoom;
    pan.y = screen_anchor.y - canvas_p0.y - world_before.y * zoom;
    flyto_active = false;
}

void VehicleMapCamera::StartFlyTo(const ImVec2& world_pos, float target_zoom)
{
    if (canvas_sz.x <= 1.0f) return;
    flyto_from_pan = pan;
    flyto_from_zoom = zoom;
    flyto_to_zoom = target_zoom;
    // Pan that puts world_pos at canvas center for the target zoom.
    flyto_to_pan = ImVec2(canvas_sz.x * 0.5f - world_pos.x * target_zoom,
                          canvas_sz.y * 0.5f - world_pos.y * target_zoom);
    flyto_t = 0.0f;
    flyto_active = true;
}

void VehicleMapCamera::UpdateFlyTo(float dt)
{
    if (!flyto_active) return;
    flyto_t += dt * 4.0f; // ~0.25s
    float t = std::min(1.0f, flyto_t);
    const float e = t * t * (3.0f - 2.0f * t); // smoothstep
    pan = ImVec2(flyto_from_pan.x + (flyto_to_pan.x - flyto_from_pan.x) * e,
                 flyto_from_pan.y + (flyto_to_pan.y - flyto_from_pan.y) * e);
    zoom = flyto_from_zoom + (flyto_to_zoom - flyto_from_zoom) * e;
    if (t >= 1.0f) flyto_active = false;
}
