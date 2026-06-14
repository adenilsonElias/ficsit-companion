#pragma once

#include <imgui.h>

/// @brief 2D pan/zoom camera for the vehicle map canvas.
///
/// Owns the view transform (pan + zoom), the per-frame canvas geometry, and the
/// fly-to animation. Pure logic: no ImGui context or rendering calls — the host
/// app feeds it canvas geometry and a per-frame delta time. This makes the
/// transform/fit/fly-to math unit-testable without a window.
class VehicleMapCamera
{
public:
    static constexpr float kDefaultZoom = 0.02f;

    // ---- Transforms ----
    ImVec2 WorldToScreen(const ImVec2& world) const;
    ImVec2 ScreenToWorld(const ImVec2& screen) const;

    // ---- Per-frame canvas geometry ----
    void SetCanvas(const ImVec2& p0, const ImVec2& size);
    const ImVec2& CanvasP0() const { return canvas_p0; }
    const ImVec2& CanvasSize() const { return canvas_sz; }

    // ---- View framing / interaction ----
    /// @brief Fit the given world bounds into the current canvas (no-op without bounds).
    void FitAll(const ImVec2& world_min, const ImVec2& world_max, bool has_bounds);
    /// @brief Translate the view by a screen-space delta (cancels fly-to).
    void PanBy(const ImVec2& delta);
    /// @brief Zoom about a screen anchor, keeping the world point under it fixed (cancels fly-to).
    void ZoomAbout(const ImVec2& screen_anchor, float wheel);

    // ---- Fly-to animation ----
    void StartFlyTo(const ImVec2& world_pos, float target_zoom);
    void UpdateFlyTo(float dt);
    bool FlyToActive() const { return flyto_active; }
    void CancelFlyTo() { flyto_active = false; }

    // ---- Accessors (for persistence gather/scatter) ----
    const ImVec2& Pan() const { return pan; }
    float Zoom() const { return zoom; }
    void SetPan(const ImVec2& p) { pan = p; }
    void SetZoom(float z) { zoom = z; }

private:
    ImVec2 pan{ 0.0f, 0.0f };
    float zoom = kDefaultZoom;

    ImVec2 canvas_p0{ 0.0f, 0.0f };
    ImVec2 canvas_sz{ 0.0f, 0.0f };

    bool flyto_active = false;
    ImVec2 flyto_from_pan{ 0.0f, 0.0f };
    float flyto_from_zoom = kDefaultZoom;
    ImVec2 flyto_to_pan{ 0.0f, 0.0f };
    float flyto_to_zoom = kDefaultZoom;
    float flyto_t = 0.0f;
};
