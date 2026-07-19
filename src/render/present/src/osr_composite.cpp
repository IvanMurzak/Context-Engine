// The OSR composite pass — see osr_composite.h.

#include "context/render/present/osr_composite.h"

#include <algorithm>

namespace context::render::present
{
namespace
{

// Fullscreen-triangle composite. The vertex stage generates the 3 oversized-triangle vertices (no
// vertex buffer), maps clip space to [0,1] with the vertical axis FLIPPED (browser rows are
// top-down), then remaps into the visible sub-rect the uniform carries.
constexpr const char* kCompositeWgsl = R"WGSL(
struct Uv {
    rect : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uv_rect : Uv;
@group(0) @binding(1) var osr_texture : texture_2d<f32>;
@group(0) @binding(2) var osr_sampler : sampler;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index : u32) -> VsOut {
    var corners = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0));
    let xy = corners[vertex_index];
    var out : VsOut;
    out.position = vec4<f32>(xy, 0.0, 1.0);
    let t = vec2<f32>((xy.x + 1.0) * 0.5, (1.0 - xy.y) * 0.5);
    out.uv = vec2<f32>(
        mix(uv_rect.rect.x, uv_rect.rect.z, t.x),
        mix(uv_rect.rect.y, uv_rect.rect.w, t.y));
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    // Already premultiplied by the producer: emit as-is and let ONE/INV_SRC_ALPHA do the fold.
    return textureSample(osr_texture, osr_sampler, in.uv);
}
)WGSL";

std::uint8_t fold_channel(std::uint8_t src, std::uint8_t dst, std::uint32_t inv_alpha)
{
    // out = src + dst * (1 - src_alpha), in 8-bit unorm with round-to-nearest.
    const std::uint32_t folded =
        static_cast<std::uint32_t>(src) + (static_cast<std::uint32_t>(dst) * inv_alpha + 127u) / 255u;
    return static_cast<std::uint8_t>(std::min<std::uint32_t>(folded, 255u));
}

} // namespace

CompositeUv compute_composite_uv(const Rect2D& visible_rect, Extent2D coded_size)
{
    CompositeUv uv;
    if (coded_size.width == 0 || coded_size.height == 0)
    {
        return uv;
    }
    if (visible_rect.size.width == 0 || visible_rect.size.height == 0)
    {
        return uv; // no visible rect reported yet — the whole allocation is the frame
    }
    const Rect2D clipped = [&]
    {
        Rect2D r;
        if (visible_rect.origin.x >= coded_size.width || visible_rect.origin.y >= coded_size.height)
        {
            r.size = coded_size;
            return r;
        }
        r.origin = visible_rect.origin;
        r.size.width = std::min(visible_rect.size.width, coded_size.width - visible_rect.origin.x);
        r.size.height = std::min(visible_rect.size.height, coded_size.height - visible_rect.origin.y);
        return r;
    }();

    const float coded_w = static_cast<float>(coded_size.width);
    const float coded_h = static_cast<float>(coded_size.height);
    uv.u0 = static_cast<float>(clipped.origin.x) / coded_w;
    uv.v0 = static_cast<float>(clipped.origin.y) / coded_h;
    uv.u1 = static_cast<float>(clipped.origin.x + clipped.size.width) / coded_w;
    uv.v1 = static_cast<float>(clipped.origin.y + clipped.size.height) / coded_h;
    return uv;
}

CompositeUniforms make_composite_uniforms(const CompositeUv& uv)
{
    CompositeUniforms out;
    out.u0 = uv.u0;
    out.v0 = uv.v0;
    out.u1 = uv.u1;
    out.v1 = uv.v1;
    return out;
}

const char* composite_wgsl()
{
    return kCompositeWgsl;
}

RenderPipelineDesc make_composite_pipeline_desc(TextureFormat target_format)
{
    RenderPipelineDesc desc;
    desc.wgsl = kCompositeWgsl;
    desc.vertex_entry = "vs_main";
    desc.fragment_entry = "fs_main";
    desc.color_format = target_format;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.blend = premultiplied_alpha_blend();
    // No depth: the UI layer is drawn last, over everything.
    return desc;
}

void composite_reference_cpu(std::vector<std::uint8_t>& dst, Extent2D dst_size,
                             const std::uint8_t* src_bgra, Extent2D coded_size,
                             std::uint32_t src_bytes_per_row, const Rect2D& visible_rect)
{
    const std::size_t needed = static_cast<std::size_t>(dst_size.width) * dst_size.height * 4u;
    if (src_bgra == nullptr || dst.size() < needed || dst_size.width == 0 || dst_size.height == 0 ||
        coded_size.width == 0 || coded_size.height == 0)
    {
        return;
    }

    const CompositeUv uv = compute_composite_uv(visible_rect, coded_size);
    const float su0 = uv.u0 * static_cast<float>(coded_size.width);
    const float sv0 = uv.v0 * static_cast<float>(coded_size.height);
    const float span_x = (uv.u1 - uv.u0) * static_cast<float>(coded_size.width);
    const float span_y = (uv.v1 - uv.v0) * static_cast<float>(coded_size.height);

    for (std::uint32_t y = 0; y < dst_size.height; ++y)
    {
        // Pixel centres, matching the rasterizer's sample positions.
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(dst_size.height);
        const auto sy = static_cast<std::uint32_t>(std::min(
            static_cast<float>(coded_size.height - 1), std::max(0.0f, sv0 + ty * span_y)));
        for (std::uint32_t x = 0; x < dst_size.width; ++x)
        {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(dst_size.width);
            const auto sx = static_cast<std::uint32_t>(std::min(
                static_cast<float>(coded_size.width - 1), std::max(0.0f, su0 + tx * span_x)));

            const std::uint8_t* s =
                src_bgra + static_cast<std::size_t>(sy) * src_bytes_per_row + sx * 4u;
            // Source memory is BGRA; a WebGPU sampler returns logical RGBA from a bgra8unorm
            // texture, so the oracle performs the same swizzle the hardware does for free.
            const std::uint8_t sr = s[2];
            const std::uint8_t sg = s[1];
            const std::uint8_t sb = s[0];
            const std::uint8_t sa = s[3];
            const std::uint32_t inv_alpha = 255u - static_cast<std::uint32_t>(sa);

            std::uint8_t* d = dst.data() + (static_cast<std::size_t>(y) * dst_size.width + x) * 4u;
            d[0] = fold_channel(sr, d[0], inv_alpha);
            d[1] = fold_channel(sg, d[1], inv_alpha);
            d[2] = fold_channel(sb, d[2], inv_alpha);
            d[3] = fold_channel(sa, d[3], inv_alpha);
        }
    }
}

} // namespace context::render::present
