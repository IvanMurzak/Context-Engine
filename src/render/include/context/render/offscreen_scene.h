// The offscreen render + pixel-readback proof, expressed ENTIRELY against the rhi.h abstraction so
// the SAME code runs on the fake backend (local ctest, no GPU) and the real wgpu-native backend (CI
// offscreen readback). It draws one clear-colored frame with a single red triangle into an offscreen
// RGBA8 texture, copies it to a mappable buffer, reads the pixels back, and asserts the background
// color, an interior triangle pixel, and the analytic coverage. This is the render-side proof the
// M4 foundation ships (R-REND-001/002); its expectations match the throwaway webgpu spike's, which
// measured byte-identical images through Naga (native) and Tint (browser).

#pragma once

#include "context/render/rhi.h"

#include <cstdint>
#include <cstdio>

namespace context::render
{

// A trivial red-triangle shader in WGSL (the sole web-path shader language, R-REND-005). Clip-space
// vertices (0,0.5)/(-0.5,-0.5)/(0.5,-0.5); solid red fragment. Backend translation is Naga (native
// wgpu) / Tint (browser).
inline const char* offscreen_triangle_wgsl()
{
    return R"(
@vertex
fn vs_main(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {
    var p = array<vec2f, 3>(
        vec2f( 0.0,  0.5),
        vec2f(-0.5, -0.5),
        vec2f( 0.5, -0.5));
    return vec4f(p[i], 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
    return vec4f(1.0, 0.0, 0.0, 1.0);
}
)";
}

enum class OffscreenResult
{
    Pass,
    Fail,
};

namespace detail
{

inline bool pixel_near(const std::uint8_t* px, int r, int g, int b, int a, int tolerance)
{
    auto close = [tolerance](int actual, int expected)
    {
        const int d = actual - expected;
        return d >= -tolerance && d <= tolerance;
    };
    return close(px[0], r) && close(px[1], g) && close(px[2], b) && close(px[3], a);
}

} // namespace detail

// Render the reference triangle offscreen through `device` and assert the readback. Returns Pass /
// Fail. `device` must be a live GPU (or fake) device; adapter presence / headless SKIP is the
// caller's concern.
inline OffscreenResult render_offscreen_triangle(IDevice& device)
{
    constexpr std::uint32_t kWidth = 256;
    constexpr std::uint32_t kHeight = 256;
    constexpr std::uint32_t kBpp = 4;
    // bytesPerRow must be a multiple of 256 (WebGPU COPY_BYTES_PER_ROW_ALIGNMENT); 256*4 = 1024.
    constexpr std::uint32_t kBytesPerRow = kWidth * kBpp;

    TextureDesc tex_desc;
    tex_desc.size = {kWidth, kHeight};
    tex_desc.format = TextureFormat::RGBA8Unorm;
    tex_desc.render_attachment = true;
    tex_desc.copy_src = true;
    std::unique_ptr<ITexture> texture = device.create_texture(tex_desc);
    std::unique_ptr<ITextureView> view = texture->create_view();

    BufferDesc buf_desc;
    buf_desc.size = static_cast<std::uint64_t>(kBytesPerRow) * kHeight;
    buf_desc.copy_dst = true;
    buf_desc.map_read = true;
    std::unique_ptr<IBuffer> readback = device.create_buffer(buf_desc);

    RenderPipelineDesc pipe_desc;
    pipe_desc.wgsl = offscreen_triangle_wgsl();
    pipe_desc.color_format = TextureFormat::RGBA8Unorm;
    std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

    std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();

    RenderPassDesc pass_desc;
    ColorAttachment attach;
    attach.view = view.get();
    attach.load = LoadOp::Clear;
    attach.store = StoreOp::Store;
    attach.clear = Color{0.1, 0.2, 0.3, 1.0};
    pass_desc.color.push_back(attach);
    {
        std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
        pass->set_pipeline(*pipeline);
        pass->draw(3, 1);
        pass->end();
    }

    TexelCopyBufferLayout layout;
    layout.bytes_per_row = kBytesPerRow;
    layout.rows_per_image = kHeight;
    encoder->copy_texture_to_buffer(*texture, *readback, layout, {kWidth, kHeight});

    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    device.queue().submit(*commands);

    const auto* pixels = static_cast<const std::uint8_t*>(readback->map_read());
    if (pixels == nullptr)
    {
        std::fprintf(stderr, "[render-offscreen] FAIL: buffer map failed\n");
        return OffscreenResult::Fail;
    }

    // (a) background pixel — clear 0.1/0.2/0.3 -> ~26/51/77 in unorm8 (rounding may differ by 1).
    const std::uint8_t* bg = pixels + (8u * kBytesPerRow) + (8u * kBpp);
    const bool bg_ok = detail::pixel_near(bg, 26, 51, 77, 255, 6);

    // (b) a pixel well inside the triangle (centroid ~ (128,149)) — solid red.
    const std::uint8_t* tri = pixels + (150u * kBytesPerRow) + (128u * kBpp);
    const bool tri_ok = detail::pixel_near(tri, 255, 0, 0, 255, 2);

    // (c) analytic coverage — the triangle covers 1/8 of the clip square = 12.5%; accept 11..14.
    std::uint32_t red = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
    {
        const std::uint8_t* row = pixels + (static_cast<std::size_t>(y) * kBytesPerRow);
        for (std::uint32_t x = 0; x < kWidth; ++x)
        {
            const std::uint8_t* px = row + (static_cast<std::size_t>(x) * kBpp);
            if (px[0] > 200 && px[1] < 60 && px[2] < 60)
            {
                ++red;
            }
        }
    }
    const double coverage = 100.0 * red / (static_cast<double>(kWidth) * kHeight);
    const bool coverage_ok = coverage > 11.0 && coverage < 14.0;

    std::printf("[render-offscreen] background(8,8)   = %u,%u,%u,%u -> %s\n", bg[0], bg[1], bg[2],
                bg[3], bg_ok ? "ok" : "MISMATCH");
    std::printf("[render-offscreen] triangle(128,150) = %u,%u,%u,%u -> %s\n", tri[0], tri[1], tri[2],
                tri[3], tri_ok ? "ok" : "MISMATCH");
    std::printf("[render-offscreen] red coverage      = %.2f%% (expected 12.50%%, accept 11..14)\n",
                coverage);

    readback->unmap();

    if (bg_ok && tri_ok && coverage_ok)
    {
        std::printf("[render-offscreen] PASS\n");
        return OffscreenResult::Pass;
    }
    std::fprintf(stderr, "[render-offscreen] FAIL: readback did not match expectations\n");
    return OffscreenResult::Fail;
}

} // namespace context::render
