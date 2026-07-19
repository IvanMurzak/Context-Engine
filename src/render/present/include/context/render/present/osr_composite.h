// The OSR composite pass — a fullscreen-triangle, PREMULTIPLIED-alpha draw of the browser's UI layer
// over whatever the window compositor has already rendered (design 03 §4, step 3).
//
// Three things have to agree for the composite to be correct, and all three live here so they cannot
// drift apart:
//
//   1. BLEND — ONE / INV_SRC_ALPHA on colour AND alpha. CEF hands over PREMULTIPLIED pixels, so the
//      source is added, not lerped; using SRC_ALPHA/INV_SRC_ALPHA (the un-premultiplied classic)
//      double-darkens every antialiased edge in the UI.
//   2. UV — the sampled sub-rect is `visible_rect / coded_size`, NOT [0,1]. The producer's allocation
//      is >= the visible area (it is resized in chunks), so sampling the whole texture stretches
//      garbage from the unused margin across the window.
//   3. ORIENTATION — the browser's rows are top-down while clip space is y-up, so the vertical UV is
//      flipped in the vertex stage.
//
// `composite_reference_cpu` is the GPU-free oracle for exactly this arithmetic: it is what pins the
// blend + UV + orientation locally on a GPU-less host (this repo's CPU-analytic-mirror pattern), and
// the SAME expectations are asserted against a real adapter by the `osr` mode of the wgpu offscreen
// proof (ctest `render-wgpu-osr-composite`).

#pragma once

#include "context/render/rhi.h"

#include <cstdint>
#include <vector>

namespace context::render::present
{

// The sampled sub-rect in normalized texture coordinates.
struct CompositeUv
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// visible_rect / coded_size, clipped to the allocation. An empty visible rect (a producer that has
// not reported one yet) yields the full [0,1] range, which is the right default for the common
// visible == coded case.
[[nodiscard]] CompositeUv compute_composite_uv(const Rect2D& visible_rect, Extent2D coded_size);

// The uniform block the composite shader reads — MUST match `struct Uv` in composite_wgsl().
struct CompositeUniforms
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

[[nodiscard]] CompositeUniforms make_composite_uniforms(const CompositeUv& uv);

// The WGSL for the fullscreen-triangle composite (bindings: 0 = uv uniform, 1 = texture, 2 = sampler).
[[nodiscard]] const char* composite_wgsl();

// The pipeline the composite pass draws with: the shader above, premultiplied-alpha blending, no
// depth. `target_format` is the attachment's format — BGRA8Unorm when compositing straight into a
// swapchain backbuffer, RGBA8Unorm for the offscreen proof.
[[nodiscard]] RenderPipelineDesc make_composite_pipeline_desc(TextureFormat target_format);

// The fullscreen triangle is 3 vertices, generated in the vertex stage (no vertex buffer).
inline constexpr std::uint32_t kCompositeVertexCount = 3;

// The GPU-free reference: composite a PREMULTIPLIED BGRA8 source over an RGBA8 destination, sampling
// the `visible_rect` sub-rect of a `coded_size` allocation with nearest filtering — the exact
// arithmetic the shader + blend state perform. `dst` is modified in place and must already hold
// dst_size.width * dst_size.height * 4 bytes.
void composite_reference_cpu(std::vector<std::uint8_t>& dst, Extent2D dst_size,
                             const std::uint8_t* src_bgra, Extent2D coded_size,
                             std::uint32_t src_bytes_per_row, const Rect2D& visible_rect);

} // namespace context::render::present
