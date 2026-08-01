// Minimal 3D math for the lit/PBR path (R-REND-004): column-major Mat4 + Vec3, look-at and
// orthographic projections targeting the WebGPU clip conventions the T1 RHI draws through.
//
// PROMOTED (M9 e11a): every type and function this header used to DEFINE now lives one layer up, in
// context/render/math.h, so that context/render/view.h -- a context_render header -- can name a Mat4
// without context_render depending on context_render_lit, which links against it. Nothing about the
// math changed; ortho() additionally guards a zero-extent box the way sprite::ortho always has.
//
// This header is kept, and re-exports each promoted name into context::render::lit, so every lit
// call site (pbr.h, lit_scene.h, their tests) compiles unchanged and `#include
// "context/render/lit/lit_math.h"` keeps meaning what it meant. New code should include
// context/render/math.h directly; the promoted home is where perspective(), inverse(),
// determinant() and rotation_from_quaternion() were added.

#pragma once

#include "context/render/math.h"

namespace context::render::lit
{

using render::Mat4;
using render::Vec3;

using render::add;
using render::cross;
using render::dot;
using render::length;
using render::look_at;
using render::mul;
using render::normalize;
using render::ortho;
using render::scale;
using render::sub;
using render::transform_point;

} // namespace context::render::lit
