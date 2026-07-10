// Minimal 3D math for the lit/PBR path (R-REND-004): column-major Mat4 + Vec3, look-at and
// orthographic projections targeting the WebGPU clip conventions the T1 RHI draws through
// (x,y in [-1,1] y-up, z in [0,1] — the same conventions the sprite path's 2D ortho pins down in
// src/render/sprite/ortho.h; this module needs the full 3D forms: look-at view, 3D point transform).
// Pure CPU and dependency-free of any GPU backend, so it builds + is unit-tested under every
// toolchain including the local Ninja+Strawberry-GCC Windows dev gate.

#pragma once

#include <array>
#include <cmath>

namespace context::render::lit
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] inline Vec3 add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] inline Vec3 sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] inline Vec3 scale(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

[[nodiscard]] inline Vec3 mul(Vec3 a, Vec3 b) // component-wise
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

[[nodiscard]] inline float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline float length(Vec3 a)
{
    return std::sqrt(dot(a, a));
}

// normalize() of a (near-)zero vector returns the zero vector rather than NaN — callers that must
// reject a degenerate direction check length() first (the extract does; see extract.cpp).
[[nodiscard]] inline Vec3 normalize(Vec3 a)
{
    const float len = length(a);
    if (!(len > 0.0f))
    {
        return {0.0f, 0.0f, 0.0f};
    }
    return scale(a, 1.0f / len);
}

// A column-major 4x4 matrix (WGSL mat4x4<f32> memory order, matching sprite::Mat4): column c, row r
// lives at m[c * 4 + r]. Identity by default.
struct Mat4
{
    std::array<float, 16> m{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    [[nodiscard]] float at(int col, int row) const
    {
        return m[static_cast<std::size_t>(col) * 4 + static_cast<std::size_t>(row)];
    }
};

// c = a * b (apply b first, then a — the usual proj * view composition).
[[nodiscard]] Mat4 mul(const Mat4& a, const Mat4& b);

// Transform a 3D point (w = 1) and return the xyz after the w-divide (affine matrices here have
// w' = 1, so the divide is a no-op for look_at/ortho — kept for generality).
[[nodiscard]] Vec3 transform_point(const Mat4& m, Vec3 p);

// A right-handed look-at VIEW matrix: camera at `eye` looking toward `center`, `up` the world-space
// up hint (must not be parallel to the view direction). View space: -Z is forward, +X right, +Y up.
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up);

// Orthographic projection of the view-space box onto the WebGPU clip cube (x,y in [-1,1] y-up,
// z in [0,1]) — the 3D analog of sprite::ortho, with near/far measured along -Z (view forward).
[[nodiscard]] Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z);

} // namespace context::render::lit
