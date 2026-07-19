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

// The sampled sub-rect in normalized texture coordinates. This IS the uniform block the composite
// shader reads — four consecutive floats matching `struct Uv { rect : vec4<f32> }` in
// composite_wgsl(), so it is uploaded directly with no repacking step to keep in sync.
struct CompositeUv
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// The shader-layout invariant, asserted rather than commented: a WGSL vec4<f32> is 16 bytes, and a
// mismatch here would upload silently-misaligned UVs.
static_assert(sizeof(CompositeUv) == 16u,
              "CompositeUv must match WGSL `struct Uv { rect : vec4<f32> }`");

// visible_rect / coded_size, clipped to the allocation. Every DEGENERATE input — an empty coded
// size, an unreported (empty) visible rect, or an origin that a concurrent resize has left outside
// the allocation — yields the full [0,1] range, which is the right default for the common
// visible == coded case and shows the whole allocation rather than a black window.
//
// Note this deliberately differs from clip_rect() in osr_import.h, which returns an EMPTY rect for
// its own out-of-bounds case. The asymmetry is intended: skipping an UPLOAD of a stale rect is
// free, whereas compositing nothing would blank the editor's UI for that frame.
[[nodiscard]] CompositeUv compute_composite_uv(const Rect2D& visible_rect, Extent2D coded_size);

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
