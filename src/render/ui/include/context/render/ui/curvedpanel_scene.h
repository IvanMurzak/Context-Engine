// The `ui-curvedpanel` golden corpus scene (M7 a10, R-UI-003; lock D4; owner ruling d; issue #241).
// The a9 world-space RTT UI panel generalized from a FLAT quad to a CURVED (cylinder-section) mesh:
// the panel content is rendered into the SAME per-panel dynamic-texture RTT (a9's dynamic_texture.h),
// then sampled across a UV-mapped curved mesh (the a10 panel-mesh seam, panel_mesh.h) placed by the
// scene's ortho camera over the same lit-ground base. Two layers composite into one 256x256 target:
//   * the flat lit-ground quad + sky clear (reused verbatim from worldpanel_scene.h) as the base, then
//   * the panel's dynamic texture sampled onto the curved mesh, painted on top.
//
// NATIVE-ONLY, per the a9 precedent (worldpanel_scene.h): the world-space RTT path rides
// dynamic_texture.cpp, which the Emscripten web golden target does NOT compile (it ships only
// triangle3d + sprite2d + ui-hud). So `ui-curvedpanel` is a native-blocking golden (goldens/manifest.json);
// browser coverage joins when the lit/world-space web proof lands (src/render/web/README.md follow-ups).
//
// Two render paths share the ONE mesh + scene geometry (CPU-computed, so both agree):
//   * render_golden_curvedpanel / render_offscreen_curvedpanel — the GPU path (lavapipe native): the
//     SSIM-gated golden + the CI readback assert;
//   * render_curvedpanel_reference_cpu — the analytic, GPU-free rasterization of the SAME projected
//     mesh triangles (opaque flat ground + nearest-sampled panel texels), which GENERATES the committed
//     baseline and is the fake-backend oracle (the GPU quad path is a no-op on the fake backend, so the
//     CPU mirror is the local pixel oracle, exactly as ui-hud / sprite2d / ui-worldpanel).
//
// Straight-on ORTHOGRAPHIC projection (worldpanel_scene.h's world_to_clip, HALF = 1.28), so each mesh
// triangle projects affinely and its UV interpolation is linear — the CPU mirror SSIM-matches the GPU
// rasterizer. The convex cylinder section (facing +Z) foreshortens toward its edges: the visible curve.

#pragma once

#include "context/packages/ui/curved_panel.h"     // PanelMesh / build_cylinder_panel_mesh
#include "context/render/ui/panel_mesh.h"          // PanelMeshBinding — the a10 mesh+UV seam
#include "context/render/ui/worldpanel_scene.h"    // reuse the RTT content, ground/sky, ortho, CPU raster

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

// The curved panel's placement in world space: a vertical cylinder SECTION (single row of quads)
// facing +Z (the camera), centred upper-of-centre so it floats above the lit ground. Segments set the
// tessellation (more = smoother curve); radius sets the projected width, height the vertical extent,
// arc the wrap angle. Kept < 180 deg so the projected columns stay monotonic (no self-overlap under
// the depth-less painter composite).
[[nodiscard]] constexpr std::uint32_t curvedpanel_segments()
{
    return 12;
}
[[nodiscard]] inline float curvedpanel_radius()
{
    return 0.9f;
}
[[nodiscard]] inline float curvedpanel_height()
{
    return 0.85f;
}
[[nodiscard]] inline float curvedpanel_arc_radians()
{
    return 2.4435f; // ~140 degrees
}
[[nodiscard]] inline packages::ui::Vec3 curvedpanel_center()
{
    return packages::ui::Vec3{0.0f, 0.3f, 0.0f};
}

// The curved panel mesh (world-space positions + the panel-scoped UV set). The SAME builder the a10
// picking path (raycast_panel.h) uses, so the rendered surface and the pickable surface are identical.
[[nodiscard]] inline packages::ui::PanelMesh curvedpanel_mesh()
{
    return packages::ui::build_cylinder_panel_mesh(curvedpanel_segments(), curvedpanel_radius(),
                                                   curvedpanel_height(), curvedpanel_arc_radians(),
                                                   curvedpanel_center());
}

// The view-pitch (radians) about the X axis, applied to the mesh at projection time: a straight-on
// ortho view of a vertical cylinder section has a rectangular silhouette (the curvature only shows as
// non-uniform column spacing), so tilting the view makes the circular cross-section project to a
// visible ARC (the top/bottom edges bow). Ortho still drops z linearly, so each triangle projects
// AFFINELY — the CPU mirror keeps matching the GPU rasterizer within the SSIM tolerance.
[[nodiscard]] inline float curvedpanel_view_pitch()
{
    return 0.38f; // ~22 degrees
}

// Project a world-space mesh point through the view pitch, then the worldpanel ortho world_to_clip.
// The ONE projection both the GPU shader geometry and the CPU mirror consume, so they agree.
[[nodiscard]] inline sprite::Vec2 curvedpanel_project(const packages::ui::Vec3& w)
{
    const float pitch = curvedpanel_view_pitch();
    const float cy = curvedpanel_center().y;
    const float cz = curvedpanel_center().z + curvedpanel_radius() * 0.6f; // pivot near the mesh depth
    const float s = std::sin(pitch);
    const float c = std::cos(pitch);
    const float ry = (w.y - cy) * c - (w.z - cz) * s + cy; // rotate about X; ortho drops the rotated z
    return detail::world_to_clip(w.x, ry);
}

namespace detail
{

// One projected mesh vertex: the (view-pitched) ortho clip-space position + its UV.
struct CurvedVert
{
    sprite::Vec2 clip;
    float u = 0.0f;
    float v = 0.0f;
};

// Project every triangle's three vertices to clip space, carrying UV — the shared geometry the GPU
// shader and the CPU raster both consume (so they agree bit-for-bit up to the rasterizer edge rule).
[[nodiscard]] inline std::vector<CurvedVert> project_curvedpanel(const packages::ui::PanelMesh& mesh)
{
    std::vector<CurvedVert> out;
    out.reserve(mesh.triangles.size() * 3u);
    for (const std::array<std::uint32_t, 3>& tri : mesh.triangles)
    {
        for (const std::uint32_t idx : tri)
        {
            const packages::ui::PanelVertex& vtx = mesh.vertices[idx];
            CurvedVert cv;
            cv.clip = curvedpanel_project(vtx.pos);
            cv.u = vtx.uv.x;
            cv.v = vtx.uv.y;
            out.push_back(cv);
        }
    }
    return out;
}

// A textured-mesh WGSL shader: `verts.size()` vertices (3 per triangle) drawn as a triangle-list,
// sampling group0 binding0 texture through binding1 sampler, multiplied by `tint`. The generalization
// of worldpanel_scene.h's single-quad worldpanel_textured_quad_wgsl to an arbitrary projected mesh.
[[nodiscard]] inline std::string curved_textured_mesh_wgsl(const std::vector<CurvedVert>& verts,
                                                           const float tint[4])
{
    std::ostringstream w;
    w.imbue(std::locale::classic()); // force '.' decimals — a non-classic locale would emit invalid WGSL
    w.setf(std::ios::fixed);
    w.precision(6);
    const std::size_t n = verts.size();
    w << "@group(0) @binding(0) var tex : texture_2d<f32>;\n"
      << "@group(0) @binding(1) var samp : sampler;\n"
      << "struct VsOut { @builtin(position) pos : vec4f, @location(0) uv : vec2f };\n"
      << "@vertex\n"
      << "fn vs_main(@builtin(vertex_index) i : u32) -> VsOut {\n"
      << "    var p = array<vec4f, " << n << ">(\n";
    for (std::size_t k = 0; k < n; ++k)
    {
        w << "        vec4f(" << verts[k].clip.x << ", " << verts[k].clip.y << ", 0.0, 1.0)"
          << (k + 1 < n ? ",\n" : ");\n");
    }
    w << "    var uv = array<vec2f, " << n << ">(\n";
    for (std::size_t k = 0; k < n; ++k)
    {
        w << "        vec2f(" << verts[k].u << ", " << verts[k].v << ")"
          << (k + 1 < n ? ",\n" : ");\n");
    }
    w << "    var o : VsOut;\n"
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

} // namespace detail

// Build the a10 mesh binding: the curved mesh bound to a freshly-rendered panel-content RTT (the a9
// teal+amber content, reused). Returns the binding + keeps the registry alive via the caller. The
// panel-mesh seam (panel_mesh.h) ties the mesh's UV set to the dynamic-texture handle.
[[nodiscard]] inline PanelMeshBinding render_curvedpanel_binding(IDevice& device,
                                                                DynamicTextureRegistry& registry)
{
    const DynamicTextureId handle = render_worldpanel_rtt(device, registry); // teal panel + amber inner
    return bind_panel_mesh(curvedpanel_mesh(), handle);
}

// The projected pixel of the panel mesh's centroid (samples UV ~0.5,0.5 -> the amber inner rect) — the
// readback probe point, computed from the SAME geometry so it follows any tweak.
inline void curvedpanel_centroid_px(float& px, float& py)
{
    const packages::ui::Vec3 c = panel_mesh_centroid(curvedpanel_mesh());
    detail::clip_to_px(curvedpanel_project(c), px, py);
}

// ---------------------------------------------------------------------------------------- GPU path

// Render the ui-curvedpanel composite offscreen through `device` into `out` (raw RGBA8, rows
// top-first, worldpanel_target_size()^2 * 4). Layer 1 (Clear): the lit-ground base quad over the sky
// clear (reused from worldpanel_scene.h); layer 2 (Load): the panel's RTT texture sampled onto the
// curved mesh. Returns false only when the readback map fails.
inline bool render_curvedpanel_pixels(IDevice& device, std::vector<std::uint8_t>& out)
{
    constexpr std::uint32_t kW = worldpanel_target_size();
    constexpr std::uint32_t kH = worldpanel_target_size();
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = kW * kBpp;

    // --- the RTT step: render the panel content into a dynamic-texture registry target -------------
    DynamicTextureRegistry registry(device);
    const PanelMeshBinding binding = render_curvedpanel_binding(device, registry);
    ITexture* panel_texture = registry.get(binding.texture);

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

    // Layer 2 (overlay): sample the panel RTT onto the curved mesh, painted on top (Load).
    {
        const std::vector<detail::CurvedVert> verts = detail::project_curvedpanel(binding.mesh);
        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = detail::curved_textured_mesh_wgsl(verts, binding.tint);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);

        SamplerDesc samp_desc; // nearest/clamp — crisp texels, backend-stable
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
            pass->draw(static_cast<std::uint32_t>(verts.size()), 1);
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
        std::fprintf(stderr, "[render-curvedpanel] FAIL: buffer map failed\n");
        return false;
    }
    out.assign(pixels, pixels + static_cast<std::size_t>(kBytesPerRow) * kH);
    readback->unmap();
    return true;
}

// The golden dump path (same frame the proof asserts, so the committed golden IS the frame).
inline bool render_golden_curvedpanel(IDevice& device, golden::GoldenImage& out)
{
    out.width = worldpanel_target_size();
    out.height = worldpanel_target_size();
    return render_curvedpanel_pixels(device, out.rgba);
}

// ------------------------------------------------------------------------------------- CPU mirror

// The analytic, GPU-free composite: fill the sky, rasterize the lit-ground trapezoid (flat), then each
// curved-mesh triangle sampling the analytic panel content (nearest). This IS the committed baseline
// (generated locally, no GPU) and the fake-backend oracle — the GPU render (lavapipe) SSIM-matches it.
inline void render_curvedpanel_reference_cpu(golden::GoldenImage& out)
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

    // Ground trapezoid (flat lit color), two triangles bl,br,tr / bl,tr,tl (reused from worldpanel).
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

    // Panel: the analytic RTT content sampled (nearest) across each projected curved-mesh triangle.
    {
        std::vector<std::uint8_t> content;
        std::uint32_t csize = 0;
        detail::panel_content_cpu(content, csize);
        const packages::ui::PanelMesh mesh = curvedpanel_mesh();
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // tint == white (panel shows its own colors)
        auto sample = [&](float u, float v, std::uint8_t* p)
        {
            int tx = static_cast<int>(u * static_cast<float>(csize));
            int ty = static_cast<int>(v * static_cast<float>(csize));
            tx = std::min(std::max(tx, 0), static_cast<int>(csize) - 1);
            ty = std::min(std::max(ty, 0), static_cast<int>(csize) - 1);
            const std::uint8_t* t =
                content.data() + (static_cast<std::size_t>(ty) * csize + static_cast<std::uint32_t>(tx)) * 4u;
            p[0] = detail::unorm8(static_cast<float>(t[0]) / 255.0f * white[0]);
            p[1] = detail::unorm8(static_cast<float>(t[1]) / 255.0f * white[1]);
            p[2] = detail::unorm8(static_cast<float>(t[2]) / 255.0f * white[2]);
            p[3] = 255u;
        };
        for (const std::array<std::uint32_t, 3>& tri : mesh.triangles)
        {
            const packages::ui::PanelVertex& va = mesh.vertices[tri[0]];
            const packages::ui::PanelVertex& vb = mesh.vertices[tri[1]];
            const packages::ui::PanelVertex& vc = mesh.vertices[tri[2]];
            const detail::ScreenVert a = to_screen(curvedpanel_project(va.pos), va.uv.x, va.uv.y);
            const detail::ScreenVert b = to_screen(curvedpanel_project(vb.pos), vb.uv.x, vb.uv.y);
            const detail::ScreenVert c = to_screen(curvedpanel_project(vc.pos), vc.uv.x, vc.uv.y);
            detail::raster_tri(out.rgba, size, a, b, c, sample);
        }
    }
}

// Render the ui-curvedpanel composite through `device` and assert the readback proves the chain: the
// sky where nothing covers, the lit ground in its band, and the panel's inner (amber) texel sampled at
// the curved mesh's projected centroid — i.e. the RTT panel texture reached the curved surface.
// `device` must actually rasterize (the wgpu backend on CI); on the GPU-free fake backend the quad
// passes are no-ops (only the sky clear lands), so this assert is the CI (lavapipe) proof — the
// fake-backend coverage is the CPU mirror (render_curvedpanel_reference_cpu). Returns true on a pass.
inline bool render_offscreen_curvedpanel(IDevice& device)
{
    golden::GoldenImage img;
    if (!render_golden_curvedpanel(device, img))
    {
        std::fprintf(stderr, "[render-curvedpanel] FAIL: composite readback failed\n");
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
    curvedpanel_centroid_px(panel_x, panel_y);
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
                                   inner.r, inner.g, inner.b, 14); // the RTT texel on the curved surface

    std::printf("[render-curvedpanel] sky(4,4)=%s ground(%d,%d)=%s panel(%d,%d)=%s\n",
                sky_ok ? "ok" : "MISMATCH", static_cast<int>(ground_x), static_cast<int>(ground_y),
                ground_ok ? "ok" : "MISMATCH", static_cast<int>(panel_x), static_cast<int>(panel_y),
                panel_ok ? "ok/RTT-on-curve" : "MISMATCH");

    const bool ok = sky_ok && ground_ok && panel_ok;
    if (ok)
    {
        std::printf("[render-curvedpanel] PASS\n");
    }
    else
    {
        std::fprintf(stderr,
                     "[render-curvedpanel] FAIL: composite readback did not match (sky=%d ground=%d "
                     "panel=%d)\n",
                     sky_ok, ground_ok, panel_ok);
    }
    return ok;
}

} // namespace context::render::ui
