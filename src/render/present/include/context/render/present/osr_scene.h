// The OSR composite GPU proof (M9 e03 DoD: "composite output pixel-asserted offscreen").
//
// Expressed entirely against rhi.h, and structured the way this repo's other render proofs are: a
// deterministic synthetic frame, rendered through the REAL backend, then compared against the CPU
// oracle in osr_composite.h. The oracle is what makes the assertion meaningful — it is independently
// pinned by the local `render-present-test_osr_composite` ctest, so "GPU agrees with oracle" plus
// "oracle is correct" chains into a real proof of the composite rather than a self-consistent tautology.
//
// The fixture is deliberately adversarial about the two mistakes this pass is most likely to make:
//   * the region OUTSIDE visible_rect is filled with a colour that appears nowhere in the expected
//     output, so sampling [0,1] instead of visible_rect/coded_size is loudly wrong, not subtly off;
//   * the visible region is half OPAQUE and half 50%-PREMULTIPLIED, so a SrcAlpha/InvSrcAlpha blend
//     (the classic un-premultiplied mistake) diverges on the translucent half while the opaque half
//     still looks fine.
//
// build_reference_osr_frame() is pure CPU and carries no device, so the local GPU-less ctest compiles
// this whole header and asserts the fixture — the GCC gate is not blind to it.

#pragma once

#include "context/render/offscreen_scene.h" // detail::pixel_near — the shared proof-pixel compare
#include "context/render/present/osr_composite.h"
#include "context/render/present/osr_import.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace context::render::present
{

// The synthetic producer frame + the target geometry the proof renders into.
struct ReferenceOsrFrame
{
    std::vector<std::uint8_t> pixels; // BGRA8, premultiplied, coded_size * 4 per row
    Extent2D coded_size;
    Rect2D visible_rect;
    std::uint32_t bytes_per_row = 0;
    Extent2D target_size; // the composite target — equal to visible_rect, so sampling is 1:1
    Color clear;          // the backdrop the UI layer is folded over
};

// The producer's allocation is DELIBERATELY larger than the visible area, which is the normal CEF
// case and the whole reason the composite divides UVs by coded_size.
[[nodiscard]] inline ReferenceOsrFrame build_reference_osr_frame()
{
    ReferenceOsrFrame frame;
    frame.coded_size = {128, 64};
    // Every one of the four UV components is DISTINCT (u0=.125 v0=.0625 u1=.625 v1=.4375), and the
    // origin is non-zero. That is deliberate and load-bearing: with a {0,0} origin and a rect whose
    // width/coded_width happens to equal its height/coded_height, a shader that dropped the origin
    // or swapped u with v would sample the identical region and the proof would pass anyway.
    frame.visible_rect.origin = {16, 4};
    frame.visible_rect.size = {64, 24};
    frame.target_size = frame.visible_rect.size; // 64 * 4 = 256 == the readback row alignment
    frame.bytes_per_row = frame.coded_size.width * 4u;
    frame.clear = Color{0.0, 0.0, 1.0, 1.0}; // opaque blue backdrop

    frame.pixels.assign(static_cast<std::size_t>(frame.bytes_per_row) * frame.coded_size.height, 0u);
    const Rect2D& vis = frame.visible_rect;
    for (std::uint32_t y = 0; y < frame.coded_size.height; ++y)
    {
        for (std::uint32_t x = 0; x < frame.coded_size.width; ++x)
        {
            std::uint8_t* p =
                frame.pixels.data() + static_cast<std::size_t>(y) * frame.bytes_per_row + x * 4u;
            const bool visible = x >= vis.origin.x && x < vis.origin.x + vis.size.width &&
                                 y >= vis.origin.y && y < vis.origin.y + vis.size.height;
            if (!visible)
            {
                // Opaque magenta: appears in the expected output NOWHERE, so a wrong UV sub-rect
                // shows up as a blatant colour rather than a plausible one.
                p[0] = 0xFF; // B
                p[1] = 0x00; // G
                p[2] = 0xFF; // R
                p[3] = 0xFF; // A
                continue;
            }
            // FOUR quadrants, so the pattern varies on BOTH axes. A vertical flip (the one thing
            // the vertex stage does to the UV) swaps the top pair with the bottom pair, which is
            // invisible to any fixture whose rows are all alike.
            const bool left = (x - vis.origin.x) < vis.size.width / 2u;
            const bool top = (y - vis.origin.y) < vis.size.height / 2u;
            if (top && left)
            {
                p[0] = 0x00; // opaque green
                p[1] = 0xFF;
                p[2] = 0x00;
                p[3] = 0xFF;
            }
            else if (top)
            {
                // 50% white, PREMULTIPLIED: colour channels are 128, not 255.
                p[0] = 0x80;
                p[1] = 0x80;
                p[2] = 0x80;
                p[3] = 0x80;
            }
            else if (left)
            {
                p[0] = 0x00; // opaque red
                p[1] = 0x00;
                p[2] = 0xFF;
                p[3] = 0xFF;
            }
            else
            {
                // Fully transparent, premultiplied: every channel zero, so this quadrant must come
                // out as the pure backdrop — the alpha=0 end of the ONE/INV_SRC_ALPHA fold.
                p[0] = 0x00;
                p[1] = 0x00;
                p[2] = 0x00;
                p[3] = 0x00;
            }
        }
    }
    return frame;
}

// The expected composite, computed by the CPU oracle over the same fixture + backdrop.
[[nodiscard]] inline std::vector<std::uint8_t> expected_osr_composite(const ReferenceOsrFrame& frame)
{
    std::vector<std::uint8_t> expected(
        static_cast<std::size_t>(frame.target_size.width) * frame.target_size.height * 4u, 0u);
    const auto to_u8 = [](double c)
    { return static_cast<std::uint8_t>(c * 255.0 + 0.5); };
    for (std::size_t i = 0; i + 3 < expected.size(); i += 4)
    {
        expected[i + 0] = to_u8(frame.clear.r);
        expected[i + 1] = to_u8(frame.clear.g);
        expected[i + 2] = to_u8(frame.clear.b);
        expected[i + 3] = to_u8(frame.clear.a);
    }
    composite_reference_cpu(expected, frame.target_size, frame.pixels.data(), frame.coded_size,
                            frame.bytes_per_row, frame.visible_rect);
    return expected;
}

// Import the reference frame, composite it over the clear colour through the real backend, read the
// pixels back and assert they match the oracle. Returns true on pass.
[[nodiscard]] inline bool render_offscreen_osr_composite(IDevice& device)
{
    const ReferenceOsrFrame frame = build_reference_osr_frame();

    // force_software makes this proof take the SAME import path on every OS, so the composite is
    // what is under test here rather than the platform's import tier (which osr_import's own ctest
    // covers exhaustively).
    OsrImportOptions options;
    options.force_software = true;
    OsrTextureImporter importer(current_present_platform(), options);

    OsrFrame producer;
    producer.pixels = frame.pixels.data();
    producer.byte_size = frame.pixels.size();
    producer.bytes_per_row = frame.bytes_per_row;
    producer.coded_size = frame.coded_size;
    producer.visible_rect = frame.visible_rect;
    if (!importer.update(device, producer) || importer.texture() == nullptr)
    {
        std::fprintf(stderr, "[render-wgpu] FAIL: OSR import: %s\n", importer.diagnostic().c_str());
        return false;
    }

    TextureDesc target_desc;
    target_desc.size = frame.target_size;
    target_desc.format = TextureFormat::RGBA8Unorm;
    target_desc.render_attachment = true;
    target_desc.copy_src = true;
    std::unique_ptr<ITexture> target = device.create_texture(target_desc);
    std::unique_ptr<ITextureView> target_view = target->create_view();
    std::unique_ptr<ITextureView> osr_view = importer.texture()->create_view();

    SamplerDesc sampler_desc; // Nearest: the target matches visible_rect 1:1, so no filtering blur
    std::unique_ptr<ISampler> sampler = device.create_sampler(sampler_desc);

    const CompositeUv uniforms = compute_composite_uv(frame.visible_rect, frame.coded_size);
    BufferDesc uniform_desc;
    uniform_desc.size = sizeof(CompositeUv);
    uniform_desc.uniform = true;
    uniform_desc.copy_dst = true;
    std::unique_ptr<IBuffer> uniform_buffer = device.create_buffer(uniform_desc);
    device.queue().write_buffer(*uniform_buffer, 0, &uniforms, sizeof(uniforms));

    std::unique_ptr<IRenderPipeline> pipeline =
        device.create_render_pipeline(make_composite_pipeline_desc(TextureFormat::RGBA8Unorm));
    std::unique_ptr<IBindGroupLayout> layout = pipeline->bind_group_layout(0);
    std::vector<BindGroupEntry> entries(3);
    entries[0].binding = 0;
    entries[0].buffer = uniform_buffer.get();
    entries[0].buffer_size = sizeof(CompositeUv);
    entries[1].binding = 1;
    entries[1].texture = osr_view.get();
    entries[2].binding = 2;
    entries[2].sampler = sampler.get();
    std::unique_ptr<IBindGroup> bind_group = device.create_bind_group(*layout, entries);

    const std::uint32_t bytes_per_row = frame.target_size.width * 4u; // 256 — already aligned
    BufferDesc readback_desc;
    readback_desc.size = static_cast<std::uint64_t>(bytes_per_row) * frame.target_size.height;
    readback_desc.copy_dst = true;
    readback_desc.map_read = true;
    std::unique_ptr<IBuffer> readback = device.create_buffer(readback_desc);

    std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
    RenderPassDesc pass;
    ColorAttachment attachment;
    attachment.view = target_view.get();
    attachment.load = LoadOp::Clear;
    attachment.clear = frame.clear;
    pass.color.push_back(attachment);
    std::unique_ptr<IRenderPassEncoder> render_pass = encoder->begin_render_pass(pass);
    render_pass->set_pipeline(*pipeline);
    render_pass->set_bind_group(0, *bind_group);
    render_pass->draw(kCompositeVertexCount, 1);
    render_pass->end();

    TexelCopyBufferLayout copy_layout;
    copy_layout.bytes_per_row = bytes_per_row;
    copy_layout.rows_per_image = frame.target_size.height;
    encoder->copy_texture_to_buffer(*target, *readback, copy_layout, frame.target_size);
    std::unique_ptr<ICommandBuffer> commands = encoder->finish();
    device.queue().submit(*commands);

    const void* mapped = readback->map_read();
    if (mapped == nullptr)
    {
        std::fprintf(stderr, "[render-wgpu] FAIL: OSR composite readback map failed\n");
        return false;
    }
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);
    const std::vector<std::uint8_t> expected = expected_osr_composite(frame);

    // Tolerance covers the unavoidable gap between the GPU's float blend and the oracle's integer
    // arithmetic; it is far tighter than the distance to either failure mode the fixture provokes
    // (a magenta sample, or the ~64-per-channel error of an un-premultiplied blend).
    constexpr int kTolerance = 3;
    int mismatches = 0;
    for (std::uint32_t y = 0; y < frame.target_size.height; ++y)
    {
        for (std::uint32_t x = 0; x < frame.target_size.width; ++x)
        {
            const std::uint8_t* got = pixels + static_cast<std::size_t>(y) * bytes_per_row + x * 4u;
            const std::uint8_t* want =
                expected.data() + (static_cast<std::size_t>(y) * frame.target_size.width + x) * 4u;
            // The verdict comes from the ONE shared per-channel tolerance compare every sibling GPU
            // proof uses (lit / sprite / viewport); only the per-channel REPORT is local, because
            // the shared helper returns a bool and "which channel" is what makes a failure legible.
            if (detail::pixel_near(got, want[0], want[1], want[2], want[3], kTolerance))
            {
                continue;
            }
            if (mismatches < 8)
            {
                for (int c = 0; c < 4; ++c)
                {
                    const int delta = static_cast<int>(got[c]) - static_cast<int>(want[c]);
                    if (delta > kTolerance || delta < -kTolerance)
                    {
                        std::fprintf(stderr,
                                     "[render-wgpu] FAIL: OSR composite (%u,%u) channel %d: got %d, "
                                     "want %d\n",
                                     x, y, c, static_cast<int>(got[c]), static_cast<int>(want[c]));
                        break;
                    }
                }
            }
            ++mismatches;
        }
    }
    readback->unmap();

    if (mismatches != 0)
    {
        std::fprintf(stderr, "[render-wgpu] FAIL: OSR composite mismatched %d pixel(s)\n",
                     mismatches);
        return false;
    }
    std::printf("[render-wgpu] PASS: OSR composite (%ux%u premultiplied over a %ux%u allocation)\n",
                frame.target_size.width, frame.target_size.height, frame.coded_size.width,
                frame.coded_size.height);
    return true;
}

} // namespace context::render::present
