// Shared CPU 3D math for the render side: column-major Mat4 + Vec3, the view/projection builders
// every render path composes, and the INVERSE direction (M9 e11a) the viewport work needs.
//
// PROMOTED from src/render/lit/lit_math.h (M9 e11a). The types and the forward-direction functions
// are the SAME ones the lit path has used since M4 (R-REND-004) -- they moved up one layer, into
// context_render, for a layering reason: context/render/view.h is a context_render header, and a
// View has to name a Mat4. Leaving Mat4 in context_render_lit would make context_render's own public
// surface depend on a library that links AGAINST it. lit_math.h now re-exports every name here, so
// the lit path's own call sites are unchanged.
//
// Conventions (the WebGPU clip space the T1 RHI draws through, R-REND-001 / L-11):
//   * Right-handed world space, y-UP. View space: -Z is forward, +X right, +Y up.
//   * Clip space is x,y in [-1,1] y-UP and z in [0,1] -- the orthoZO / perspectiveZO depth range,
//     NOT GL's [-1,1]. The sprite path's 2D ortho (context/render/sprite/ortho.h) pins the same one.
//   * Column-major (the std / WGSL mat4x4<f32> memory order): column c, row r lives at m[c * 4 + r],
//     and a point is transformed as `M * vec4(p, 1)`.
//
// Pure CPU and dependency-free of any GPU backend, so it builds + is unit-tested under every
// toolchain including the local Ninja+Strawberry-GCC Windows dev gate.

#pragma once

#include <array>
#include <cmath>

namespace context::render
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

// normalize() of a (near-)zero vector returns the zero vector rather than NaN -- callers that must
// reject a degenerate direction check length() first (the extract does; see extract.cpp). This is
// the house rule the degenerate guards below all follow: return something FINITE and document how a
// caller asks whether the input was degenerate, rather than propagating an inf/NaN.
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

// c = a * b (apply b first, then a -- the usual proj * view composition).
[[nodiscard]] Mat4 mul(const Mat4& a, const Mat4& b);

// Transform a 3D point (w = 1) and return the xyz after the w-divide. Affine matrices (look_at,
// ortho) leave w' = 1 so the divide is a no-op there; a PERSPECTIVE matrix does not, which is why
// the divide is not optional -- project()/unproject() in view.h both rely on it. A w' of exactly 0
// (a point on the projective plane at infinity) is left UNDIVIDED rather than yielding an infinity,
// the same house rule as normalize().
[[nodiscard]] Vec3 transform_point(const Mat4& m, Vec3 p);

// A right-handed look-at VIEW matrix: camera at `eye` looking toward `center`, `up` the world-space
// up hint (must not be parallel to the view direction). View space: -Z is forward, +X right, +Y up.
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up);

// Orthographic projection of the view-space box onto the WebGPU clip cube (x,y in [-1,1] y-up,
// z in [0,1]) -- the 3D analog of sprite::ortho, with near/far measured along -Z (view forward).
//
// A degenerate (zero-extent) box yields a FINITE identity-scale matrix on the collapsed axis rather
// than a divide-by-zero, matching sprite::ortho's guard: a zero-sized viewport is a real transient
// state (a panel mid-resize, a minimized window) and must not poison the frame with NaNs.
[[nodiscard]] Mat4 ortho(float left, float right, float bottom, float top, float near_z,
                         float far_z);

// A right-handed PERSPECTIVE projection onto the same WebGPU clip cube (x,y in [-1,1] y-up,
// z in [0,1]) -- the projective sibling of ortho() above, and the piece the M4 lit/sprite paths
// never needed because both frame their scenes orthographically. `fov_y_radians` is the FULL
// vertical field of view, `aspect` is width/height. The frustum is SYMMETRIC by construction (an
// off-centre principal point is not expressible through fov + aspect and no caller needs one yet).
//
// Degenerate inputs (a non-positive or non-finite aspect, a fov at or past the half-turn, a
// zero-depth range) fall back to finite defaults on that axis rather than yielding inf/NaN -- the
// same guard ortho() applies, for the same reason.
[[nodiscard]] Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z);

// The determinant of `m`. A caller that must REJECT a singular transform (rather than accept
// inverse()'s identity fallback below) tests this against its own tolerance first.
[[nodiscard]] float determinant(const Mat4& m);

// The inverse of `m` (full 4x4 cofactor expansion -- `m` is NOT assumed affine, because a
// perspective view-projection is not).
//
// A SINGULAR or non-finite `m` has no inverse: this returns the IDENTITY rather than a matrix full
// of inf/NaN, the same house rule normalize() follows for a zero vector. Callers that must
// distinguish "inverted" from "was not invertible" check determinant() first.
[[nodiscard]] Mat4 inverse(const Mat4& m);

// A rotation matrix from a quaternion (x, y, z, w) -- the component order render::Transform stores.
// The quaternion is normalized first, so an unnormalized authored value still yields a pure
// rotation; a zero-length quaternion yields the identity (the normalize() house rule again).
[[nodiscard]] Mat4 rotation_from_quaternion(float x, float y, float z, float w);

} // namespace context::render
