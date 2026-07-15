// The `ui-worldpanel` golden corpus scene (M7 a9, R-UI-003; lock D4; issue #141 golden discipline).
// A runtime UI panel rendered into a persistent per-panel offscreen render target (RTT) — the FIRST
// dynamic-texture registry entry (dynamic_texture.h) — then SAMPLED onto a flat quad rotated /
// positioned in a lit 3D scene via a render::Transform, exactly as a render_world.h UiPanel (bound to
// that dynamic-texture handle) is placed by the L-39 extract. Two layers composite into one target:
//   * a flat "lit ground" quad (a directional-light-shaded ground plane) as the 3D-scene base, then
//   * the panel's dynamic texture sampled onto the rotated world quad, painted on top.
// The layer read-back IS the golden frame, so goldens/ui-worldpanel.ppm can never drift from what the
// composite renders — the same "the golden is the proof's frame by construction" discipline as
// triangle3d / sprite2d / viewport / ui-hud.
//
// NATIVE-ONLY (like golden_lit.h's lit3d + viewport_scene.h's viewport): compiled into the native
// offscreen exe, NOT the Emscripten web target — the web golden target compiles only triangle3d +
// sprite2d today (goldens/manifest.json); browser coverage joins when the lit web proof lands.
//
// Two render paths share the ONE scene geometry (computed on the CPU, so both agree bit-for-bit):
//   * render_golden_worldpanel / render_offscreen_worldpanel — the GPU path (real adapter: lavapipe
//     native): the SSIM-gated golden + the CI readback assert;
//   * render_worldpanel_reference_cpu — the analytic, GPU-free rasterization of the SAME projected
//     quads (opaque flat fills + nearest-sampled panel texels), which GENERATES the committed baseline
//     and is asserted locally on the fake backend (the GPU quad path is a no-op on the fake backend,
//     exactly as ui-hud / sprite2d — so the CPU mirror is the local oracle and the baseline generator).
//
// The projection is a straight-on ORTHOGRAPHIC camera (no perspective divide), so a rotated quad
// projects affinely — its UV interpolation is linear and the CPU mirror matches the GPU rasterizer
// within the SSIM tolerance. World is y-up; world (x,y) maps to a 256x256 target by x_px = (x/HALF +
// 1)*128, y_px = (1 - y/HALF)*128 (row 0 = top), HALF = the world half-extent framed.

#pragma once

#include "context/packages/ui/provider.h" // RepaintPlan
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/golden.h" // golden::GoldenImage + write_ppm
#include "context/render/render_world.h"
#include "context/render/rhi.h"
#include "context/render/sprite/ortho.h"          // sprite::Vec2
#include "context/render/sprite/sprite_offscreen.h" // sprite::quad_wgsl (flat-color quad)
#include "context/render/ui/dynamic_texture.h"
#include "context/render/ui/provider.h" // GpuUiProvider
#include "context/render/ui/snapshot.h" // extract_ui / UiQuad

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace context::render::ui
{

// The composite target edge (square RGBA8), shared by the GPU render, the analytic baseline, and
// goldens/ui-worldpanel.ppm. 256 like every corpus scene.
[[nodiscard]] constexpr std::uint32_t worldpanel_target_size()
{
    return 256;
}

// The panel's RTT edge (the dynamic-texture registry target). 64 keeps the readback row 256-aligned
// (64*4 = 256) and is ample for the flat colored panel (text arrives with a7/a8).
[[nodiscard]] constexpr std::uint32_t worldpanel_rtt_size()
{
    return 64;
}

// The world half-extent the orthographic camera frames: world x,y in [-HALF, HALF] maps to [0, 256].
[[nodiscard]] constexpr float worldpanel_half_extent()
{
    return 1.28f;
}

// The sky/clear behind the 3D scene (a dark slate).
[[nodiscard]] inline Color worldpanel_clear_color()
{
    return Color{0.05, 0.06, 0.10, 1.0};
}

// The flat "lit ground" color: a directional-light Lambert shade of a mid-gray ground plane, precomputed
// to a constant (a9 ships the colored-rect scene; a full PBR base is the lit3d golden's job). Linear RGBA.
[[nodiscard]] inline std::array<float, 4> worldpanel_ground_color()
{
    return {0.22f, 0.24f, 0.30f, 1.0f};
}

// The panel content colors (packages::ui::Color bytes): a teal panel with a centered amber inner rect.
[[nodiscard]] inline packages::ui::Color worldpanel_panel_bg_color()
{
    return packages::ui::Color{40, 160, 180, 255};
}
[[nodiscard]] inline packages::ui::Color worldpanel_panel_inner_color()
{
    return packages::ui::Color{230, 190, 60, 255};
}

namespace detail
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A unit quaternion (x,y,z,w) for a rotation of `angle` radians about `axis` (normalized here).
[[nodiscard]] inline std::array<float, 4> quat_axis_angle(Vec3 axis, float angle)
{
    const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (!(len > 0.0f))
    {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float s = std::sin(angle * 0.5f) / len;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

// Hamilton product a*b (apply b first, then a).
[[nodiscard]] inline std::array<float, 4> quat_mul(const std::array<float, 4>& a,
                                                   const std::array<float, 4>& b)
{
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// Rotate `v` by unit quaternion `q` (xyzw): v' = v + 2*qxyz x (qxyz x v + qw*v).
[[nodiscard]] inline Vec3 rotate(const std::array<float, 4>& q, Vec3 v)
{
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 w = cross(u, v);
    const Vec3 t = cross(u, w); // cross(u, cross(u, v)) — reuse w, don't recompute cross(u, v)
    return {v.x + 2.0f * (q[3] * w.x + t.x), v.y + 2.0f * (q[3] * w.y + t.y),
            v.z + 2.0f * (q[3] * w.z + t.z)};
}

// World (x,y) -> the orthographic clip-space corner (x,y in [-1,1], y-up). z/depth is dropped (straight-on
// ortho, painter-order compositing, no depth test).
[[nodiscard]] inline sprite::Vec2 world_to_clip(float wx, float wy)
{
    const float h = worldpanel_half_extent();
    return sprite::Vec2{wx / h, wy / h};
}

// Clip (x,y in [-1,1], y-up) -> the 256x256 target pixel (rows top-first).
inline void clip_to_px(const sprite::Vec2& c, float& px, float& py)
{
    const float n = static_cast<float>(worldpanel_target_size());
    px = (c.x + 1.0f) * 0.5f * n;
    py = (1.0f - c.y) * 0.5f * n;
}

} // namespace detail

// The panel's authored world placement: a flat quad, scaled / rotated (yaw about Y + roll about Z, so
// the projected quad is both foreshortened AND tilted — clearly a 3D-oriented panel) / translated by a
// render::Transform, plus the UiPanel component (the `texture` handle is bound to the dynamic-texture
// registry at render time). Size (world meters) sets the base quad extent; the extract copies this
// verbatim, so the render side reconstructs exactly this quad.
struct WorldPanelPlacement
{
    Transform transform;
    UiPanel panel;
};

[[nodiscard]] inline WorldPanelPlacement worldpanel_placement()
{
    WorldPanelPlacement p;
    // Rotate 30 deg about Y (foreshorten) then 13 deg about Z (tilt) — q = qZ * qY (yaw applied first).
    const std::array<float, 4> qy = detail::quat_axis_angle(detail::Vec3{0.0f, 1.0f, 0.0f}, 0.5236f);
    const std::array<float, 4> qz = detail::quat_axis_angle(detail::Vec3{0.0f, 0.0f, 1.0f}, 0.2269f);
    const std::array<float, 4> q = detail::quat_mul(qz, qy);
    p.transform.rotation[0] = q[0];
    p.transform.rotation[1] = q[1];
    p.transform.rotation[2] = q[2];
    p.transform.rotation[3] = q[3];
    p.transform.position[0] = 0.0f;
    p.transform.position[1] = 0.25f; // float above the ground, upper-center
    p.transform.position[2] = 0.0f;
    p.transform.scale[0] = 1.0f;
    p.transform.scale[1] = 1.0f;
    p.transform.scale[2] = 1.0f;
    p.panel.size[0] = 1.4f; // world-space quad width
    p.panel.size[1] = 1.0f; // world-space quad height
    // tint stays white — the panel shows its own colors.
    return p;
}

// The four world-space corners of the placed panel quad, in [bottom-left, bottom-right, top-right,
// top-left] LOCAL order (so UV assignment follows the corner index regardless of rotation). Applies the
// Transform (scale -> rotate -> translate) to the base quad in the entity's XY plane.
[[nodiscard]] inline std::array<detail::Vec3, 4> worldpanel_world_corners(const WorldPanelPlacement& p)
{
    const float hw = p.panel.size[0] * 0.5f;
    const float hh = p.panel.size[1] * 0.5f;
    const std::array<detail::Vec3, 4> local{detail::Vec3{-hw, -hh, 0.0f}, detail::Vec3{hw, -hh, 0.0f},
                                            detail::Vec3{hw, hh, 0.0f}, detail::Vec3{-hw, hh, 0.0f}};
    const std::array<float, 4> q{p.transform.rotation[0], p.transform.rotation[1],
                                 p.transform.rotation[2], p.transform.rotation[3]};
    std::array<detail::Vec3, 4> world{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const detail::Vec3 s{local[i].x * p.transform.scale[0], local[i].y * p.transform.scale[1],
                             local[i].z * p.transform.scale[2]};
        const detail::Vec3 r = detail::rotate(q, s);
        world[i] = detail::Vec3{r.x + p.transform.position[0], r.y + p.transform.position[1],
                                r.z + p.transform.position[2]};
    }
    return world;
}

// The four clip-space corners of the placed panel quad (bl,br,tr,tl order).
[[nodiscard]] inline std::array<sprite::Vec2, 4> worldpanel_clip_corners(const WorldPanelPlacement& p)
{
    const std::array<detail::Vec3, 4> world = worldpanel_world_corners(p);
    return {detail::world_to_clip(world[0].x, world[0].y),
            detail::world_to_clip(world[1].x, world[1].y),
            detail::world_to_clip(world[2].x, world[2].y),
            detail::world_to_clip(world[3].x, world[3].y)};
}

// The lit-ground base quad's four clip corners (bl,br,tr,tl): a receding-floor trapezoid (wide near
// edge at the bottom, narrower far edge) filling the lower band — the 3D-scene ground under the panel.
[[nodiscard]] inline std::array<sprite::Vec2, 4> worldpanel_ground_clip_corners()
{
    return {detail::world_to_clip(-1.28f, -1.28f), detail::world_to_clip(1.28f, -1.28f),
            detail::world_to_clip(0.9f, -0.4f), detail::world_to_clip(-0.9f, -0.4f)};
}

// Build the panel CONTENT tree (rendered into the RTT): a teal panel with a centered amber inner rect.
inline void build_worldpanel_content(packages::ui::UiTree& tree)
{
    using packages::ui::NodeId;
    using packages::ui::Rect;
    using packages::ui::Role;
    using packages::ui::Style;

    const float s = static_cast<float>(worldpanel_rtt_size());
    const NodeId bg = tree.create_node(Role::Panel, tree.root());
    tree.set_bounds(bg, Rect{0, 0, s, s});
    Style bg_style;
    bg_style.background = worldpanel_panel_bg_color();
    tree.set_style(bg, bg_style);

    const float inset = s * 0.1875f; // 12 px at 64 -> [12,12,40,40], a centered inner rect
    const NodeId inner = tree.create_node(Role::Panel, bg);
    tree.set_bounds(inner, Rect{inset, inset, s - 2.0f * inset, s - 2.0f * inset});
    Style inner_style;
    inner_style.background = worldpanel_panel_inner_color();
    tree.set_style(inner, inner_style);
}

// The projected pixel of the panel quad's CENTROID (samples UV (0.5,0.5) -> the amber inner rect) and of
// a ground-interior point (the lit ground) — the readback probe points, computed from the SAME geometry
// so they follow any tweak.
inline void worldpanel_panel_centroid_px(float& px, float& py)
{
    const WorldPanelPlacement p = worldpanel_placement();
    detail::clip_to_px(detail::world_to_clip(p.transform.position[0], p.transform.position[1]), px, py);
}
inline void worldpanel_ground_probe_px(float& px, float& py)
{
    detail::clip_to_px(detail::world_to_clip(0.0f, -0.85f), px, py);
}

// ---------------------------------------------------------------------------------------- GPU path

// A textured-quad WGSL shader: two triangles from the four clip corners (bl,br,tr,tl) with UVs, sampling
// group0 binding0 texture through binding1 sampler, multiplied by `tint`. Mirrors sprite::quad_wgsl's
// vertex-index geometry, plus the texture sample the RTT panel needs.
[[nodiscard]] inline std::string worldpanel_textured_quad_wgsl(const std::array<sprite::Vec2, 4>& c,
                                                               const float tint[4])
{
    std::ostringstream w;
    w.imbue(std::locale::classic()); // force '.' decimals — a non-classic locale would emit invalid WGSL
    w.setf(std::ios::fixed);
    w.precision(6);
    // vertex order bl,br,tr, bl,tr,tl with matching UVs (top-left texel = UV (0,0); clip y-up top = tr/tl).
    w << "@group(0) @binding(0) var tex : texture_2d<f32>;\n"
      << "@group(0) @binding(1) var samp : sampler;\n"
      << "struct VsOut { @builtin(position) pos : vec4f, @location(0) uv : vec2f };\n"
      << "@vertex\n"
      << "fn vs_main(@builtin(vertex_index) i : u32) -> VsOut {\n"
      << "    var p = array<vec4f, 6>(\n"
      << "        vec4f(" << c[0].x << ", " << c[0].y << ", 0.0, 1.0),\n"  // bl
      << "        vec4f(" << c[1].x << ", " << c[1].y << ", 0.0, 1.0),\n"  // br
      << "        vec4f(" << c[2].x << ", " << c[2].y << ", 0.0, 1.0),\n"  // tr
      << "        vec4f(" << c[0].x << ", " << c[0].y << ", 0.0, 1.0),\n"  // bl
      << "        vec4f(" << c[2].x << ", " << c[2].y << ", 0.0, 1.0),\n"  // tr
      << "        vec4f(" << c[3].x << ", " << c[3].y << ", 0.0, 1.0));\n" // tl
      << "    var uv = array<vec2f, 6>(\n"
      << "        vec2f(0.0, 1.0), vec2f(1.0, 1.0), vec2f(1.0, 0.0),\n"
      << "        vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(0.0, 0.0));\n"
      << "    var o : VsOut;\n"
      << "    o.pos = p[i];\n"
      << "    o.uv = uv[i];\n"
      << "    return o;\n"
      << "}\n\n"
      << "@fragment\n"
      << "fn fs_main(in : VsOut) -> @location(0) vec4f {\n"
      << "    return textureSample(tex, samp, in.uv) * vec4f(" << tint[0] << ", " << tint[1] << ", "
      << tint[2] << ", " << tint[3] << ");\n"
      << "}\n";
    return w.str();
}

// Render the panel content into a dynamic-texture registry target (the RTT step) and return the handle
// + the registry (which owns the persistent target). Uses the GpuUiProvider world-space form (renders
// into the registry texture, not a self-owned layer) — the a6 backend generalized to a per-panel target.
inline DynamicTextureId render_worldpanel_rtt(IDevice& device, DynamicTextureRegistry& registry)
{
    const std::uint32_t s = worldpanel_rtt_size();
    const DynamicTextureId handle = registry.create_panel_target(Extent2D{s, s});
    ITexture* target = registry.get(handle);
    GpuUiProvider provider(device, *target, Extent2D{s, s},
                           Color{static_cast<double>(worldpanel_panel_bg_color().r) / 255.0,
                                 static_cast<double>(worldpanel_panel_bg_color().g) / 255.0,
                                 static_cast<double>(worldpanel_panel_bg_color().b) / 255.0, 1.0});
    packages::ui::UiTree tree;
    build_worldpanel_content(tree);
    packages::ui::RepaintPlan plan;
    plan.full_repaint = true;
    provider.present(tree, plan);
    return handle;
}

// Render the ui-worldpanel composite offscreen through `device` into `out` (raw RGBA8, rows top-first,
// worldpanel_target_size()^2 * 4). Layer 1 (Clear): the lit-ground base quad over the sky clear; layer 2
// (Load): the panel's RTT texture sampled onto the rotated world quad. The panel RTT is rendered first
// (render_worldpanel_rtt) then bound + sampled. Returns false only when the readback map fails. `device`
// must be a live (GPU or fake) device; adapter presence / headless SKIP is the caller's concern.
inline bool render_worldpanel_pixels(IDevice& device, std::vector<std::uint8_t>& out)
{
    constexpr std::uint32_t kW = worldpanel_target_size();
    constexpr std::uint32_t kH = worldpanel_target_size();
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = kW * kBpp; // 1024, already a 256-multiple

    // --- the RTT step: render the panel content into a dynamic-texture registry target -------------
    DynamicTextureRegistry registry(device);
    const DynamicTextureId panel_tex = render_worldpanel_rtt(device, registry);
    ITexture* panel_texture = registry.get(panel_tex);

    // --- the composite target ---------------------------------------------------------------------
    TextureDesc td;
    td.size = {kW, kH};
    td.format = TextureFormat::RGBA8Unorm;
    td.render_attachment = true;
    td.copy_src = true;
    std::unique_ptr<ITexture> target = device.create_texture(td);
    std::unique_ptr<ITextureView> target_view = target->create_view();

    BufferDesc bd;
    bd.size = static_cast<std::uint64_t>(kBytesPerRow) * kH;
    bd.copy_dst = true;
    bd.map_read = true;
    std::unique_ptr<IBuffer> readback = device.create_buffer(bd);

    const Color sky = worldpanel_clear_color();

    // Layer 1 (base): clear to sky, then draw the flat lit-ground quad (reuse the sprite flat-color quad).
    {
        const std::array<sprite::Vec2, 4> gc = worldpanel_ground_clip_corners();
        const std::array<float, 4> gcol = worldpanel_ground_color();
        const float gcol_a[4] = {gcol[0], gcol[1], gcol[2], gcol[3]};
        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = sprite::quad_wgsl(gc, gcol_a);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = target_view.get();
        attach.load = LoadOp::Clear;
        attach.store = StoreOp::Store;
        attach.clear = sky;
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

    // Layer 2 (overlay): sample the panel RTT onto the rotated world quad, painted on top (Load).
    {
        const WorldPanelPlacement placement = worldpanel_placement();
        const std::array<sprite::Vec2, 4> pc = worldpanel_clip_corners(placement);
        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = worldpanel_textured_quad_wgsl(pc, placement.panel.tint);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

        SamplerDesc samp_desc; // nearest/clamp — crisp texels, backend-stable (no filtering divergence)
        std::unique_ptr<ISampler> sampler = device.create_sampler(samp_desc);
        std::unique_ptr<ITextureView> panel_view = panel_texture->create_view();
        std::unique_ptr<IBindGroupLayout> layout = pipeline->bind_group_layout(0);
        std::vector<BindGroupEntry> entries(2);
        entries[0].binding = 0;
        entries[0].texture = panel_view.get();
        entries[1].binding = 1;
        entries[1].sampler = sampler.get();
        std::unique_ptr<IBindGroup> bind = device.create_bind_group(*layout, entries);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = target_view.get();
        attach.load = LoadOp::Load; // composite on top of the ground/sky base
        attach.store = StoreOp::Store;
        attach.clear = sky;
        pass_desc.color.push_back(attach);
        {
            std::unique_ptr<IRenderPassEncoder> pass = encoder->begin_render_pass(pass_desc);
            pass->set_pipeline(*pipeline);
            pass->set_bind_group(0, *bind);
            pass->draw(6, 1);
            pass->end();
        }
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);
    }

    // Copy the composited target to the readback buffer (its own encoder — after all draws).
    {
        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        TexelCopyBufferLayout layout;
        layout.bytes_per_row = kBytesPerRow;
        layout.rows_per_image = kH;
        encoder->copy_texture_to_buffer(*target, *readback, layout, {kW, kH});
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);
    }

    const auto* pixels = static_cast<const std::uint8_t*>(readback->map_read());
    if (pixels == nullptr)
    {
        std::fprintf(stderr, "[render-worldpanel] FAIL: buffer map failed\n");
        return false;
    }
    out.assign(pixels, pixels + static_cast<std::size_t>(kBytesPerRow) * kH);
    readback->unmap();
    return true;
}

// The golden dump path (same frame the proof asserts, so the committed golden IS the frame).
inline bool render_golden_worldpanel(IDevice& device, golden::GoldenImage& out)
{
    out.width = worldpanel_target_size();
    out.height = worldpanel_target_size();
    return render_worldpanel_pixels(device, out.rgba);
}

// ------------------------------------------------------------------------------------- CPU mirror

namespace detail
{

// Byte value of a linear [0,1] float (round-to-nearest), matching the RHI clear unorm rule.
[[nodiscard]] inline std::uint8_t unorm8(float c)
{
    if (c < 0.0f)
        c = 0.0f;
    if (c > 1.0f)
        c = 1.0f;
    return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
}

// The analytic 64x64 panel CONTENT (tight RGBA8, rows top-first): the SAME quads GpuUiProvider paints
// into the RTT, rasterized GPU-free (extract_ui + pixel-centre box fill, painter order) — the ui-hud
// analytic-baseline discipline. This is what both the GPU RTT (on lavapipe) and the CPU mirror sample.
inline void panel_content_cpu(std::vector<std::uint8_t>& content, std::uint32_t& size)
{
    size = worldpanel_rtt_size();
    content.assign(static_cast<std::size_t>(size) * size * 4u, 0u);
    const packages::ui::Color bg = worldpanel_panel_bg_color();
    for (std::size_t i = 0; i + 3 < content.size(); i += 4)
    {
        content[i + 0] = bg.r;
        content[i + 1] = bg.g;
        content[i + 2] = bg.b;
        content[i + 3] = 255u;
    }
    packages::ui::UiTree tree;
    build_worldpanel_content(tree);
    UiRenderSnapshot snap;
    extract_ui(tree, packages::ui::Rect{0, 0, static_cast<float>(size), static_cast<float>(size)}, snap);
    for (const UiQuad& quad : snap.quads) // pre-order == painter order
    {
        for (std::uint32_t y = 0; y < size; ++y)
        {
            const float cy = static_cast<float>(y) + 0.5f;
            if (cy < quad.rect.y || cy >= quad.rect.y + quad.rect.h)
                continue;
            for (std::uint32_t x = 0; x < size; ++x)
            {
                const float cx = static_cast<float>(x) + 0.5f;
                if (cx < quad.rect.x || cx >= quad.rect.x + quad.rect.w)
                    continue;
                std::uint8_t* p = content.data() + (static_cast<std::size_t>(y) * size + x) * 4u;
                p[0] = quad.color.r;
                p[1] = quad.color.g;
                p[2] = quad.color.b;
                p[3] = 255u;
            }
        }
    }
}

struct ScreenVert
{
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

// Rasterize a triangle into `out` (256x256 RGBA8), calling `shade(u,v)` per covered pixel (pixel-centre
// inside, the GPU coverage rule). Barycentric UV interpolation (affine — ortho projection, so linear).
template <typename Shade>
inline void raster_tri(std::vector<std::uint8_t>& out, std::uint32_t size, const ScreenVert& a,
                       const ScreenVert& b, const ScreenVert& c, Shade shade)
{
    const float min_x = std::min({a.x, b.x, c.x});
    const float max_x = std::max({a.x, b.x, c.x});
    const float min_y = std::min({a.y, b.y, c.y});
    const float max_y = std::max({a.y, b.y, c.y});
    const int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    const int x1 = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(max_x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    const int y1 = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(max_y)));
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f)
        return;
    const float inv_area = 1.0f / area;
    for (int y = y0; y <= y1; ++y)
    {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x)
        {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv_area;
            const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv_area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;
            const float u = w0 * a.u + w1 * b.u + w2 * c.u;
            const float v = w0 * a.v + w1 * b.v + w2 * c.v;
            std::uint8_t* p =
                out.data() + (static_cast<std::size_t>(y) * size + static_cast<std::uint32_t>(x)) * 4u;
            shade(u, v, p);
        }
    }
}

} // namespace detail

// The analytic, GPU-free composite: fill the sky, rasterize the lit-ground trapezoid (flat), then the
// rotated panel quad sampling the analytic panel content (nearest). This IS the committed baseline
// (generated locally, no GPU) and the fake-backend oracle — the GPU render (lavapipe) SSIM-matches it.
inline void render_worldpanel_reference_cpu(golden::GoldenImage& out)
{
    const std::uint32_t size = worldpanel_target_size();
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4u, 0u);

    const Color sky = worldpanel_clear_color();
    const std::uint8_t sky_r = detail::unorm8(static_cast<float>(sky.r));
    const std::uint8_t sky_g = detail::unorm8(static_cast<float>(sky.g));
    const std::uint8_t sky_b = detail::unorm8(static_cast<float>(sky.b));
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4)
    {
        out.rgba[i + 0] = sky_r;
        out.rgba[i + 1] = sky_g;
        out.rgba[i + 2] = sky_b;
        out.rgba[i + 3] = 255u;
    }

    auto to_screen = [&](const sprite::Vec2& clip, float u, float v) -> detail::ScreenVert
    {
        float px = 0.0f;
        float py = 0.0f;
        detail::clip_to_px(clip, px, py);
        return detail::ScreenVert{px, py, u, v};
    };

    // Ground trapezoid (flat lit color), two triangles bl,br,tr / bl,tr,tl.
    {
        const std::array<sprite::Vec2, 4> gc = worldpanel_ground_clip_corners();
        const std::array<float, 4> gcol = worldpanel_ground_color();
        const std::uint8_t gr = detail::unorm8(gcol[0]);
        const std::uint8_t gg = detail::unorm8(gcol[1]);
        const std::uint8_t gb = detail::unorm8(gcol[2]);
        auto fill = [&](float, float, std::uint8_t* p)
        {
            p[0] = gr;
            p[1] = gg;
            p[2] = gb;
            p[3] = 255u;
        };
        const detail::ScreenVert bl = to_screen(gc[0], 0.0f, 0.0f);
        const detail::ScreenVert br = to_screen(gc[1], 0.0f, 0.0f);
        const detail::ScreenVert tr = to_screen(gc[2], 0.0f, 0.0f);
        const detail::ScreenVert tl = to_screen(gc[3], 0.0f, 0.0f);
        detail::raster_tri(out.rgba, size, bl, br, tr, fill);
        detail::raster_tri(out.rgba, size, bl, tr, tl, fill);
    }

    // Panel quad (nearest-sampled RTT content), two triangles bl,br,tr / bl,tr,tl. UVs: bl(0,1) br(1,1)
    // tr(1,0) tl(0,0) — matching the GPU shader.
    {
        std::vector<std::uint8_t> content;
        std::uint32_t csize = 0;
        detail::panel_content_cpu(content, csize);
        const WorldPanelPlacement placement = worldpanel_placement();
        const std::array<sprite::Vec2, 4> pc = worldpanel_clip_corners(placement);
        const float tr_ = placement.panel.tint[0];
        const float tg_ = placement.panel.tint[1];
        const float tb_ = placement.panel.tint[2];
        auto sample = [&](float u, float v, std::uint8_t* p)
        {
            int tx = static_cast<int>(u * static_cast<float>(csize));
            int ty = static_cast<int>(v * static_cast<float>(csize));
            tx = std::min(std::max(tx, 0), static_cast<int>(csize) - 1);
            ty = std::min(std::max(ty, 0), static_cast<int>(csize) - 1);
            const std::uint8_t* t =
                content.data() + (static_cast<std::size_t>(ty) * csize + static_cast<std::uint32_t>(tx)) * 4u;
            p[0] = detail::unorm8(static_cast<float>(t[0]) / 255.0f * tr_);
            p[1] = detail::unorm8(static_cast<float>(t[1]) / 255.0f * tg_);
            p[2] = detail::unorm8(static_cast<float>(t[2]) / 255.0f * tb_);
            p[3] = 255u;
        };
        const detail::ScreenVert bl = to_screen(pc[0], 0.0f, 1.0f);
        const detail::ScreenVert br = to_screen(pc[1], 1.0f, 1.0f);
        const detail::ScreenVert tr = to_screen(pc[2], 1.0f, 0.0f);
        const detail::ScreenVert tl = to_screen(pc[3], 0.0f, 0.0f);
        detail::raster_tri(out.rgba, size, bl, br, tr, sample);
        detail::raster_tri(out.rgba, size, bl, tr, tl, sample);
    }
}

// Render the ui-worldpanel composite through `device` and assert the readback proves the chain: the sky
// where nothing covers, the lit ground in its band, and the panel's inner (amber) texel sampled at the
// rotated quad's centroid — i.e. the RTT panel texture reached the world quad. `device` must actually
// rasterize (the wgpu backend on CI); on the GPU-free fake backend the quad passes are no-ops (only the
// sky clear lands), so this assert is the CI (lavapipe) proof — the fake-backend coverage is the CPU
// mirror (render_worldpanel_reference_cpu). Returns true on a passing readback.
inline bool render_offscreen_worldpanel(IDevice& device)
{
    golden::GoldenImage img;
    if (!render_golden_worldpanel(device, img))
    {
        std::fprintf(stderr, "[render-worldpanel] FAIL: composite readback failed\n");
        return false;
    }
    const std::uint32_t bpr = worldpanel_target_size() * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    auto near_rgb = [](const std::uint8_t* px, int r, int g, int b, int tol)
    {
        auto ok = [tol](int a, int e) { return a - e >= -tol && a - e <= tol; };
        return ok(px[0], r) && ok(px[1], g) && ok(px[2], b);
    };

    float panel_x = 0.0f;
    float panel_y = 0.0f;
    worldpanel_panel_centroid_px(panel_x, panel_y);
    float ground_x = 0.0f;
    float ground_y = 0.0f;
    worldpanel_ground_probe_px(ground_x, ground_y);

    const packages::ui::Color inner = worldpanel_panel_inner_color();
    const std::array<float, 4> gcol = worldpanel_ground_color();

    const bool sky_ok = near_rgb(at(4, 4), 13, 15, 26, 8); // clear corner (no ground, no panel)
    const bool ground_ok = near_rgb(at(static_cast<std::uint32_t>(ground_x),
                                       static_cast<std::uint32_t>(ground_y)),
                                    detail::unorm8(gcol[0]), detail::unorm8(gcol[1]),
                                    detail::unorm8(gcol[2]), 12);
    const bool panel_ok = near_rgb(at(static_cast<std::uint32_t>(panel_x),
                                      static_cast<std::uint32_t>(panel_y)),
                                   inner.r, inner.g, inner.b, 14); // the RTT texel on the world quad

    std::printf("[render-worldpanel] sky(4,4)=%s ground(%d,%d)=%s panel(%d,%d)=%s\n",
                sky_ok ? "ok" : "MISMATCH", static_cast<int>(ground_x), static_cast<int>(ground_y),
                ground_ok ? "ok" : "MISMATCH", static_cast<int>(panel_x), static_cast<int>(panel_y),
                panel_ok ? "ok/RTT-on-quad" : "MISMATCH");

    const bool ok = sky_ok && ground_ok && panel_ok;
    if (ok)
    {
        std::printf("[render-worldpanel] PASS\n");
    }
    else
    {
        std::fprintf(stderr,
                     "[render-worldpanel] FAIL: composite readback did not match (sky=%d ground=%d "
                     "panel=%d)\n",
                     sky_ok, ground_ok, panel_ok);
    }
    return ok;
}

} // namespace context::render::ui
