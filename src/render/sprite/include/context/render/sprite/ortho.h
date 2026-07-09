// Orthographic 2D projection for the sprite path (R-2D-001, L-55).
//
// A first-class 2D draw path in the SAME renderer (L-55: NOT a 3D engine with 2D bolted on). The
// sprite path projects world-space 2D rectangles through an orthographic camera into WebGPU clip
// space and draws them over the T1 RHI (context/render/rhi.h). This header is the pure-CPU
// projection math — dependency-free of any GPU backend, so it builds + is unit-tested under every
// toolchain (including the local Ninja+Strawberry-GCC Windows dev gate) with no GPU.
//
// Conventions (match the WebGPU clip space the T1 RHI targets, R-REND-001/L-11):
//   * World space is y-UP (a rise in world y moves a sprite toward the top of the screen).
//   * Clip space (WebGPU / the RHI's shader output) is x,y in [-1, 1] with y-UP, z in [0, 1].
//   * The projection is column-major (the std / WGSL mat4x4 convention): a point p in clip space is
//     `M * vec4(world.x, world.y, z, 1)`, columns stored contiguously in `m[0..3]`, `m[4..7]`, ...

#pragma once

#include <array>

namespace context::render::sprite
{

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

// A column-major 4x4 matrix (WGSL mat4x4<f32> memory order): column c, row r lives at m[c * 4 + r].
struct Mat4
{
    std::array<float, 16> m{};

    [[nodiscard]] float at(int col, int row) const
    {
        return m[static_cast<std::size_t>(col) * 4 + static_cast<std::size_t>(row)];
    }
};

// Build an orthographic projection mapping the box [left,right] x [bottom,top] x [near,far] onto the
// WebGPU clip cube (x,y in [-1,1] y-up, z in [0,1]). Mirrors glm::orthoZO / the wgpu depth range
// (L-11: WebGPU is the baseline, so the [0,1] depth convention — not GL's [-1,1] — is the target).
[[nodiscard]] Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z);

// A 2D orthographic camera expressed as a center + half-extents (the natural 2D-editor framing: the
// camera sees `2*half_width` x `2*half_height` world units centered on `center`). `pixels_per_unit`
// is a convenience for callers that think in pixels; it does not affect the projection (framing is by
// world extents) and is carried for the sprite→world sizing helpers.
struct Camera2D
{
    Vec2 center{0.0f, 0.0f};
    float half_width = 1.0f;
    float half_height = 1.0f;
    float near_z = 0.0f;
    float far_z = 1.0f;

    [[nodiscard]] Mat4 projection() const;
};

// Project a world-space 2D point through `proj` into clip space (returns clip x,y in [-1,1] when the
// point is inside the camera box). z is taken as 0 (the sprite plane); w is 1 (affine ortho).
[[nodiscard]] Vec2 project_point(const Mat4& proj, Vec2 world);

// The four clip-space corners of an axis-aligned world rectangle centered at `center` with the given
// `size` (width,height in world units), projected through `proj`. Order: bottom-left, bottom-right,
// top-right, top-left (CCW in clip space, y-up). This is the SHARED geometry the GPU sprite-draw
// offscreen proof (sprite_offscreen.h) bakes into its WGSL, so a local unit test of these corners
// pins down exactly what the Linux-CI GPU proof rasterizes.
[[nodiscard]] std::array<Vec2, 4> quad_clip_corners(const Mat4& proj, Vec2 center, Vec2 size);

} // namespace context::render::sprite
