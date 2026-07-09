// The GPU sprite-draw offscreen + pixel-readback proof (R-2D-001), expressed ENTIRELY against the
// T1 RHI abstraction (context/render/rhi.h) — the SAME offscreen path the triangle proof uses
// (context/render/offscreen_scene.h). It draws an ortho-projected 2D scene (two overlapping sprites
// in different sorting layers) into an offscreen RGBA8 texture, reads the pixels back, and asserts:
//   * the background (clear) where no sprite covers,
//   * each sprite's flat color at its projected screen centre,
//   * in the overlap, the HIGHER sorting-layer sprite wins — proving sort order + draw order on GPU.
//
// The quad geometry is the CPU ortho projection (ortho.h::quad_clip_corners) baked into a per-sprite
// WGSL shader (the T1 RHI has no vertex-buffer binding yet, exactly as the triangle proof generates
// its vertices from vertex_index). Because the corners come from the unit-tested CPU projection, a
// green CI GPU readback here proves the ortho path places the mathematically-verified quad correctly.
//
// This is the CI-gated (Linux, like render-wgpu-offscreen) GPU proof: it needs a real adapter that
// actually rasterizes, so it runs on the wgpu-native backend. The GPU-free fake backend rasterizes
// only the reference triangle (3-vertex draw), so this proof is NOT run there — the local gate proves
// the CPU geometry/sort/batch logic instead (see the sprite tests).

#pragma once

#include "context/render/rhi.h"
#include "context/render/sprite/batch.h"
#include "context/render/sprite/ortho.h"
#include "context/render/sprite/sprite.h"

#include <cstdint>
#include <cstdio>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace context::render::sprite
{

// Build a WGSL shader that draws one axis-aligned quad from the four clip-space corners (bl,br,tr,tl)
// as two triangles generated from @builtin(vertex_index), with a flat fragment color. Mirrors the
// vertex-index geometry generation of the triangle proof's shader.
inline std::string quad_wgsl(const std::array<Vec2, 4>& c, const float color[4])
{
    std::ostringstream w;
    w.imbue(std::locale::classic()); // force '.' decimals: a non-classic locale would emit invalid WGSL
    w.setf(std::ios::fixed);
    w.precision(6);
    w << "@vertex\n"
      << "fn vs_main(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
      << "    var p = array<vec2f, 6>(\n"
      << "        vec2f(" << c[0].x << ", " << c[0].y << "),\n"  // bl
      << "        vec2f(" << c[1].x << ", " << c[1].y << "),\n"  // br
      << "        vec2f(" << c[2].x << ", " << c[2].y << "),\n"  // tr
      << "        vec2f(" << c[0].x << ", " << c[0].y << "),\n"  // bl
      << "        vec2f(" << c[2].x << ", " << c[2].y << "),\n"  // tr
      << "        vec2f(" << c[3].x << ", " << c[3].y << "));\n" // tl
      << "    return vec4f(p[i], 0.0, 1.0);\n"
      << "}\n\n"
      << "@fragment\n"
      << "fn fs_main() -> @location(0) vec4f {\n"
      << "    return vec4f(" << color[0] << ", " << color[1] << ", " << color[2] << ", " << color[3]
      << ");\n"
      << "}\n";
    return w.str();
}

// The reference 2D scene: a 256x256 offscreen view of a camera centred at the world origin with
// half-extents 128 (so 1 world unit == 1 pixel; world (0,0) maps to screen centre (128,128), y-up).
// Sprite A (red, layer 0) left of centre; Sprite B (green, layer 1) right of centre; they overlap in
// a central column where B — the higher layer — must win.
struct SpriteScene
{
    Camera2D camera;
    std::vector<Sprite2D> sprites;
};

inline SpriteScene reference_sprite_scene()
{
    SpriteScene scene;
    scene.camera.center = {0.0f, 0.0f};
    scene.camera.half_width = 128.0f;
    scene.camera.half_height = 128.0f;

    Sprite2D a;                 // red, further back
    a.position[0] = -40.0f; a.position[1] = 0.0f;
    a.size[0] = 80.0f;      a.size[1] = 80.0f;
    a.color[0] = 1.0f; a.color[1] = 0.0f; a.color[2] = 0.0f; a.color[3] = 1.0f;
    a.sort_layer = 0; a.order_in_layer = 0; a.atlas_id = 0;

    Sprite2D b;                 // green, on top
    b.position[0] = 20.0f;  b.position[1] = 0.0f;
    b.size[0] = 80.0f;      b.size[1] = 80.0f;
    b.color[0] = 0.0f; b.color[1] = 1.0f; b.color[2] = 0.0f; b.color[3] = 1.0f;
    b.sort_layer = 1; b.order_in_layer = 0; b.atlas_id = 0;

    scene.sprites = {a, b};
    return scene;
}

namespace detail
{

inline bool sprite_pixel_near(const std::uint8_t* px, int r, int g, int b, int a, int tol)
{
    auto close = [tol](int actual, int expected)
    {
        const int d = actual - expected;
        return d >= -tol && d <= tol;
    };
    return close(px[0], r) && close(px[1], g) && close(px[2], b) && close(px[3], a);
}

} // namespace detail

// Render the reference sprite scene offscreen through `device` and assert the readback. `device` must
// be a live GPU device that actually rasterizes (the wgpu backend on CI); adapter presence / headless
// SKIP is the caller's concern. Returns true on a passing readback.
inline bool render_offscreen_sprite(IDevice& device)
{
    constexpr std::uint32_t kW = 256;
    constexpr std::uint32_t kH = 256;
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = kW * kBpp; // 1024, already a 256-multiple

    const SpriteScene scene = reference_sprite_scene();
    const Mat4 proj = scene.camera.projection();
    const std::vector<std::uint32_t> draw_order = sort_draw_order(scene.sprites);

    const Color clear{0.1, 0.2, 0.3, 1.0};

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

    // Draw each sprite (in draw order) as its own render pass into the same texture: the first pass
    // clears, the rest LOAD (composite on top). Pass submission order == draw order == painter order,
    // so the higher-layer sprite naturally overdraws the lower one in the overlap.
    bool first = true;
    for (std::uint32_t idx : draw_order)
    {
        const Sprite2D& s = scene.sprites[idx];
        const std::array<Vec2, 4> corners =
            quad_clip_corners(proj, Vec2{s.position[0], s.position[1]}, Vec2{s.size[0], s.size[1]});

        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = quad_wgsl(corners, s.color);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = view.get();
        attach.load = first ? LoadOp::Clear : LoadOp::Load;
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
        first = false;
    }

    // Copy the final texture to the readback buffer (its own encoder — after all draws are submitted).
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
        std::fprintf(stderr, "[render-sprite] FAIL: buffer map failed\n");
        return false;
    }

    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return pixels + (static_cast<std::size_t>(row) * kBytesPerRow) + (col * kBpp); };

    // (a) background — a corner no sprite covers -> the clear color (~26/51/77).
    const bool bg_ok = detail::sprite_pixel_near(at(8, 8), 26, 51, 77, 255, 6);
    // (b) sprite A only (red) — screen (60,128), left of the overlap.
    const bool red_ok = detail::sprite_pixel_near(at(60, 128), 255, 0, 0, 255, 4);
    // (c) sprite B only (green) — screen (170,128), right of the overlap.
    const bool green_ok = detail::sprite_pixel_near(at(170, 128), 0, 255, 0, 255, 4);
    // (d) overlap — screen (118,128): B (layer 1) must win over A (layer 0). THE sort-order assertion.
    const bool overlap_ok = detail::sprite_pixel_near(at(118, 128), 0, 255, 0, 255, 4);

    const std::uint8_t* ov = at(118, 128);
    std::printf("[render-sprite] bg(8,8)=%u,%u,%u,%u red(60,128)=%s green(170,128)=%s "
                "overlap(118,128)=%u,%u,%u,%u(%s)\n",
                at(8, 8)[0], at(8, 8)[1], at(8, 8)[2], at(8, 8)[3], red_ok ? "ok" : "MISMATCH",
                green_ok ? "ok" : "MISMATCH", ov[0], ov[1], ov[2], ov[3],
                overlap_ok ? "ok/B-on-top" : "MISMATCH");

    readback->unmap();

    const bool ok = bg_ok && red_ok && green_ok && overlap_ok;
    if (ok)
    {
        std::printf("[render-sprite] PASS\n");
    }
    else
    {
        std::fprintf(stderr, "[render-sprite] FAIL: readback did not match (bg=%d red=%d green=%d "
                             "overlap=%d)\n",
                     bg_ok, red_ok, green_ok, overlap_ok);
    }
    return ok;
}

} // namespace context::render::sprite
