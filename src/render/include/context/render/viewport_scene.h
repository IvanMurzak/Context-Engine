// The observer-viewport composite scene (M5-F1, issue #164; R-REND-002 T1 semantics, L-41). Renders
// the editor viewport's "live scene (3D+2D)" as ONE frame through ANY rhi.h device — the native wgpu
// backend, or the GPU-free fake test backend — by compositing the two kernel-free T1 corpus scenes
// into a single offscreen target:
//   * the 3D clip-space triangle (offscreen_scene.h) as the base layer (a Clear pass), then
//   * the R-2D-001 ortho sprites (sprite_offscreen.h) painted ON TOP (Load passes, painter order),
// exactly as the observer viewport composites a 3D scene with a 2D sprite overlay. The frame is the
// SAME shared-WGSL render path both native (Naga) and the browser (Tint) run for triangle3d/sprite2d,
// so equivalence WITHIN THE T1 FEATURE SET follows from those web-gated primitives — this composite is
// the native golden ("viewport") the M5-F1 viewport panel proves its render against (goldens/), the
// same native-only pattern as golden_lit.h's lit3d.
//
// The T1 primitives use NO alpha blending and NO MSAA (opaque replace, binary coverage), so a covered
// pixel is exactly the topmost primitive's flat color and the composite is deterministic across
// backends — the committed goldens/viewport.ppm is byte-identical to compositing the committed
// triangle3d.ppm + sprite2d.ppm (sprite-over-triangle-over-clear), which the CI render job verifies.
//
// KERNEL-FREE (like golden.h): the triangle + sprite paths pull no kernel/extract surface. Consumers
// need the sprite package on the include path (native: link context_render_sprite) — already true for
// the offscreen exe and the viewport panel that reference this composite's shape.

#pragma once

#include "context/render/golden.h"
#include "context/render/offscreen_scene.h"
#include "context/render/rhi.h"
#include "context/render/sprite/ortho.h"
#include "context/render/sprite/sprite_offscreen.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace context::render
{

// The observer-viewport composite target edge (square RGBA8), shared by the proof assertion, the
// golden-scene corpus dump, and the committed goldens/viewport.ppm baseline. Equal to the triangle +
// sprite corpus edge (256) — the layers composite 1:1 into the same target.
[[nodiscard]] constexpr std::uint32_t viewport_target_size()
{
    return offscreen_triangle_size();
}

// Render the observer-viewport composite (3D triangle base + 2D sprites overlaid) offscreen through
// `device` and return the raw RGBA8 readback in `out` (row-major, rows top-first,
// viewport_target_size()^2 * 4 bytes). One target texture: a first Clear pass draws the 3D triangle,
// then each sprite (in sort/draw order) is painted on top with a Load pass — the same painter order
// sprite_offscreen.h uses, so the 2D layer overdraws the 3D layer wherever it covers. Returns false
// only when the readback map fails. `device` must be a live GPU (or fake) device; adapter presence /
// headless SKIP is the caller's concern.
inline bool render_offscreen_viewport_pixels(IDevice& device, std::vector<std::uint8_t>& out)
{
    constexpr std::uint32_t kW = viewport_target_size();
    constexpr std::uint32_t kH = viewport_target_size();
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = kW * kBpp; // 1024, already a 256-multiple

    const Color clear{0.1, 0.2, 0.3, 1.0}; // the shared corpus clear — matches triangle3d + sprite2d

    TextureDesc td;
    td.size = {kW, kH};
    td.format = TextureFormat::RGBA8Unorm;
    td.render_attachment = true;
    td.copy_src = true;
    std::unique_ptr<ITexture> texture = device.create_texture(td);
    std::unique_ptr<ITextureView> view = texture->create_view();

    BufferDesc bd;
    bd.size = static_cast<std::uint64_t>(kBytesPerRow) * kH;
    bd.copy_dst = true;
    bd.map_read = true;
    std::unique_ptr<IBuffer> readback = device.create_buffer(bd);

    // Layer 1 (base): the 3D clip-space triangle — a Clear pass (clears then draws the 3-vertex
    // triangle), the SAME shader/geometry the triangle3d golden renders.
    {
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
        attach.clear = clear;
        pass_desc.color.push_back(attach);
        {
            std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
            pass->set_pipeline(*pipeline);
            pass->draw(3, 1);
            pass->end();
        }
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);
    }

    // Layer 2 (overlay): the R-2D-001 ortho sprites painted ON TOP (Load passes, painter order == draw
    // order), the SAME scene/geometry the sprite2d golden renders. Opaque replace, so the higher
    // sorting-layer sprite wins in the overlap and the sprites overdraw the triangle where they cover.
    const sprite::SpriteScene scene = sprite::reference_sprite_scene();
    const sprite::Mat4 proj = scene.camera.projection();
    const std::vector<std::uint32_t> draw_order = sprite::sort_draw_order(scene.sprites);
    for (std::uint32_t idx : draw_order)
    {
        const sprite::Sprite2D& s = scene.sprites[idx];
        const std::array<sprite::Vec2, 4> corners = sprite::quad_clip_corners(
            proj, sprite::Vec2{s.position[0], s.position[1]}, sprite::Vec2{s.size[0], s.size[1]});

        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = sprite::quad_wgsl(corners, s.color);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = view.get();
        attach.load = LoadOp::Load; // composite on top of the triangle layer
        attach.store = StoreOp::Store;
        attach.clear = clear;
        pass_desc.color.push_back(attach);
        {
            std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
            pass->set_pipeline(*pipeline);
            pass->draw(6, 1);
            pass->end();
        }
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);
    }

    // Copy the final composited texture to the readback buffer (its own encoder — after all draws).
    {
        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        TexelCopyBufferLayout layout;
        layout.bytes_per_row = kBytesPerRow;
        layout.rows_per_image = kH;
        encoder->copy_texture_to_buffer(*texture, *readback, layout, {kW, kH});
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);
    }

    const auto* pixels = static_cast<const std::uint8_t*>(readback->map_read());
    if (pixels == nullptr)
    {
        std::fprintf(stderr, "[render-viewport] FAIL: buffer map failed\n");
        return false;
    }
    out.assign(pixels, pixels + static_cast<std::size_t>(kBytesPerRow) * kH);
    readback->unmap();
    return true;
}

// Render the viewport composite through `device` into `out` (the golden-corpus dump path). The same
// frame render_offscreen_viewport asserts, so the committed golden IS the proof's frame by
// construction. Returns false when the readback fails.
inline bool render_golden_viewport(IDevice& device, golden::GoldenImage& out)
{
    out.width = viewport_target_size();
    out.height = viewport_target_size();
    return render_offscreen_viewport_pixels(device, out.rgba);
}

// Render the viewport composite offscreen through `device` and assert the readback proves BOTH layers
// composited: the clear background where nothing covers, the 3D triangle where only it covers, and the
// 2D sprite ON TOP where it overlaps the triangle. `device` must actually rasterize (the wgpu backend
// on CI); the GPU-free fake backend rasterizes only the 3-vertex triangle (sprite quads are Load-pass
// no-ops there), so this assert is the CI (lavapipe) proof — the fake-backend coverage is the golden
// round-trip test (test_golden_cpu.cpp). Returns true on a passing readback.
inline bool render_offscreen_viewport(IDevice& device)
{
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = viewport_target_size() * kBpp;

    std::vector<std::uint8_t> image;
    if (!render_offscreen_viewport_pixels(device, image))
    {
        return false;
    }
    const std::uint8_t* pixels = image.data();
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return pixels + (static_cast<std::size_t>(row) * kBytesPerRow) + (col * kBpp); };

    // (a) background — a corner covered by neither layer -> the clear color (~26/51/77).
    const bool bg_ok = detail::pixel_near(at(8, 8), 26, 51, 77, 255, 6);
    // (b) 3D layer only — inside the triangle, BELOW the sprites (row > 168): solid red.
    const bool tri_ok = detail::pixel_near(at(128, 180), 255, 0, 0, 255, 4);
    // (c) 2D layer over the 3D layer — sprite B (green) at its centre (150,128), where it overlaps the
    //     triangle: green must win -> proves the 2D overlay composites ON TOP of the 3D base.
    const bool sprite_ok = detail::pixel_near(at(150, 128), 0, 255, 0, 255, 4);

    const std::uint8_t* tri = at(128, 180);
    const std::uint8_t* spr = at(150, 128);
    std::printf("[render-viewport] bg(8,8)=%u,%u,%u,%u(%s) tri(128,180)=%u,%u,%u,%u(%s) "
                "sprite(150,128)=%u,%u,%u,%u(%s)\n",
                at(8, 8)[0], at(8, 8)[1], at(8, 8)[2], at(8, 8)[3], bg_ok ? "ok" : "MISMATCH",
                tri[0], tri[1], tri[2], tri[3], tri_ok ? "ok/3D" : "MISMATCH", spr[0], spr[1], spr[2],
                spr[3], sprite_ok ? "ok/2D-on-top" : "MISMATCH");

    const bool ok = bg_ok && tri_ok && sprite_ok;
    if (ok)
    {
        std::printf("[render-viewport] PASS\n");
    }
    else
    {
        std::fprintf(stderr, "[render-viewport] FAIL: composite readback did not match (bg=%d tri=%d "
                             "sprite=%d)\n",
                     bg_ok, tri_ok, sprite_ok);
    }
    return ok;
}

} // namespace context::render
