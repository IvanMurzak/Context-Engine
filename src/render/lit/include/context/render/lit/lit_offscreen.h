// The GPU lit/PBR offscreen + pixel-readback proof (R-REND-004/006), expressed ENTIRELY against
// the T1 RHI abstraction (context/render/rhi.h) — the same offscreen pattern as the triangle
// (offscreen_scene.h) and sprite (sprite_offscreen.h) proofs. It drives the REAL sim->render path:
// a kernel World is populated with light/material components, extract_render_world() snapshots it
// (R-REND-003 one-way), pack_scene_uniform() feeds the GPU — then a depth-only shadow pass + a PBR
// main pass render the reference scene, and the readback is asserted against the CPU reference
// (pbr.cpp / lit_scene.cpp) at analytically-pinned sample points:
//   * PBR shading matches the unit-tested CPU mirror at every probe point (tolerance-aware),
//   * lit-vs-unlit and light/material PARAMETER deltas respond (R-REND-004 "lighting responds"),
//   * a known-shadowed ground point is darker than its lit sibling, and equal to it once shadows
//     are disabled (the depth-pass + PCF actually occludes — AC2),
//   * the R-REND-006 lightmap INPUT hook round-trips: enabling it shifts a ground pixel by exactly
//     lightmap * albedo, and leaves the hook-free blocker untouched.
//
// Like the sprite proof this needs an adapter that actually rasterizes — the CI `render` job's
// lavapipe leg. Assertions are TOLERANCE-AWARE (never exact-pixel): lavapipe's software depth
// precision may differ from hardware, which the shadow bias + probe placement absorb.

#pragma once

#include "context/kernel/world.h"
#include "context/render/extract.h"
#include "context/render/lit/lit_scene.h"
#include "context/render/offscreen_scene.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace context::render::lit
{

namespace detail
{

// Round a linear [0,1] float to its RGBA8-unorm byte.
inline int unorm_byte(float v)
{
    if (v < 0.0f)
    {
        v = 0.0f;
    }
    if (v > 1.0f)
    {
        v = 1.0f;
    }
    return static_cast<int>(v * 255.0f + 0.5f);
}

inline int luminance(const std::uint8_t* px)
{
    return (static_cast<int>(px[0]) + px[1] + px[2]) / 3;
}

} // namespace detail

// Render the reference lit scene offscreen through `device` under several uniform variants and
// assert the readbacks. `device` must be a live, rasterizing GPU device; adapter presence /
// headless SKIP is the caller's concern. Returns true on a passing readback.
inline bool render_offscreen_lit(IDevice& device)
{
    constexpr std::uint32_t kSize = lit_target_size();
    constexpr std::uint32_t kBpp = 4;
    constexpr std::uint32_t kBytesPerRow = kSize * kBpp; // 1024 — already 256-aligned
    const Color kClear{0.02, 0.02, 0.04, 1.0};

    // ---- the sim side: authored components -> extract -> snapshot (R-REND-003 one-way) --------
    kernel::World world;
    populate_reference_world(world);
    RenderSnapshot snapshot;
    extract_render_world(world, 1u, snapshot);
    if (snapshot.items.size() != 2u || snapshot.directional_lights.size() != 1u ||
        snapshot.point_lights.size() != 1u)
    {
        std::fprintf(stderr,
                     "[render-lit] FAIL: extract shape (items=%zu dir=%zu point=%zu, want 2/1/1)\n",
                     snapshot.items.size(), snapshot.directional_lights.size(),
                     snapshot.point_lights.size());
        return false;
    }
    {
        // The authored sun direction was deliberately unnormalized — the extract must normalize.
        const float* d = snapshot.directional_lights.front().light.direction;
        const float len_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (len_sq < 0.99f || len_sq > 1.01f)
        {
            std::fprintf(stderr, "[render-lit] FAIL: extracted sun direction not unit length\n");
            return false;
        }
    }

    // ---- GPU resources -------------------------------------------------------------------------
    TextureDesc color_desc;
    color_desc.size = {kSize, kSize};
    color_desc.format = TextureFormat::RGBA8Unorm;
    color_desc.render_attachment = true;
    color_desc.copy_src = true;
    std::unique_ptr<ITexture> color_tex = device.create_texture(color_desc);
    std::unique_ptr<ITextureView> color_view = color_tex->create_view();

    TextureDesc shadow_desc;
    shadow_desc.size = {kSize, kSize};
    shadow_desc.format = TextureFormat::Depth32Float;
    shadow_desc.render_attachment = true;
    shadow_desc.texture_binding = true;
    std::unique_ptr<ITexture> shadow_tex = device.create_texture(shadow_desc);
    std::unique_ptr<ITextureView> shadow_view = shadow_tex->create_view();

    // The 2x2 constant lightmap INPUT (R-REND-006 hook), uploaded through the queue.
    TextureDesc lightmap_desc;
    lightmap_desc.size = {2, 2};
    lightmap_desc.format = TextureFormat::RGBA8Unorm;
    lightmap_desc.texture_binding = true;
    lightmap_desc.copy_dst = true;
    std::unique_ptr<ITexture> lightmap_tex = device.create_texture(lightmap_desc);
    std::unique_ptr<ITextureView> lightmap_view = lightmap_tex->create_view();
    {
        std::uint8_t texels[16];
        for (int t = 0; t < 4; ++t)
        {
            for (int c = 0; c < 4; ++c)
            {
                texels[t * 4 + c] = lightmap_texel_rgba()[c];
            }
        }
        TexelCopyBufferLayout layout;
        layout.bytes_per_row = 2 * kBpp;
        layout.rows_per_image = 2;
        device.queue().write_texture(*lightmap_tex, texels, sizeof texels, layout, {2, 2});
    }

    BufferDesc uniform_desc;
    uniform_desc.size = sizeof(SceneUniformData);
    uniform_desc.uniform = true;
    uniform_desc.copy_dst = true;
    std::unique_ptr<IBuffer> uniform_buf = device.create_buffer(uniform_desc);

    BufferDesc readback_desc;
    readback_desc.size = static_cast<std::uint64_t>(kBytesPerRow) * kSize;
    readback_desc.copy_dst = true;
    readback_desc.map_read = true;
    std::unique_ptr<IBuffer> readback = device.create_buffer(readback_desc);

    SamplerDesc shadow_sampler_desc;
    shadow_sampler_desc.min_filter = FilterMode::Linear;
    shadow_sampler_desc.mag_filter = FilterMode::Linear;
    shadow_sampler_desc.compare = CompareFunction::LessEqual; // the 2x2 PCF comparison sampler
    std::unique_ptr<ISampler> shadow_sampler = device.create_sampler(shadow_sampler_desc);

    SamplerDesc lightmap_sampler_desc; // nearest/clamp — the constant lightmap needs no filtering
    std::unique_ptr<ISampler> lightmap_sampler = device.create_sampler(lightmap_sampler_desc);

    const std::string wgsl = lit_wgsl();

    RenderPipelineDesc main_desc;
    main_desc.wgsl = wgsl;
    main_desc.vertex_entry = "vs_main";
    main_desc.fragment_entry = "fs_main";
    main_desc.color_format = TextureFormat::RGBA8Unorm;
    std::unique_ptr<IRenderPipeline> main_pipe = device.create_render_pipeline(main_desc);

    RenderPipelineDesc shadow_pipe_desc;
    shadow_pipe_desc.wgsl = wgsl;
    shadow_pipe_desc.vertex_entry = "vs_shadow";
    shadow_pipe_desc.fragment_entry = ""; // depth-only
    shadow_pipe_desc.depth = DepthState{TextureFormat::Depth32Float, true, CompareFunction::Less};
    std::unique_ptr<IRenderPipeline> shadow_pipe = device.create_render_pipeline(shadow_pipe_desc);

    // Bind groups against each pipeline's REFLECTED layout (rhi.h auto-layout; see IBindGroupLayout
    // for the T3d Tint binding-renumbering contract this model absorbs). The depth-only pipeline
    // statically uses only the uniform, so its group 0 carries just binding 0.
    std::unique_ptr<IBindGroupLayout> shadow_layout = shadow_pipe->bind_group_layout(0);
    std::vector<BindGroupEntry> shadow_entries(1);
    shadow_entries[0].binding = 0;
    shadow_entries[0].buffer = uniform_buf.get();
    std::unique_ptr<IBindGroup> shadow_bind =
        device.create_bind_group(*shadow_layout, shadow_entries);

    std::unique_ptr<IBindGroupLayout> main_layout = main_pipe->bind_group_layout(0);
    std::vector<BindGroupEntry> main_entries(5);
    main_entries[0].binding = 0;
    main_entries[0].buffer = uniform_buf.get();
    main_entries[1].binding = 1;
    main_entries[1].texture = shadow_view.get();
    main_entries[2].binding = 2;
    main_entries[2].sampler = shadow_sampler.get();
    main_entries[3].binding = 3;
    main_entries[3].texture = lightmap_view.get();
    main_entries[4].binding = 4;
    main_entries[4].sampler = lightmap_sampler.get();
    std::unique_ptr<IBindGroup> main_bind = device.create_bind_group(*main_layout, main_entries);

    // ---- one render variant: shadow depth pass -> PBR main pass -> readback --------------------
    auto render_variant = [&](const LitSceneConfig& config, std::vector<std::uint8_t>& out)
    {
        const SceneUniformData uniform = pack_scene_uniform(snapshot, config);
        device.queue().write_buffer(*uniform_buf, 0, &uniform, sizeof uniform);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        {
            RenderPassDesc pass;
            DepthAttachment depth;
            depth.view = shadow_view.get();
            depth.load = LoadOp::Clear;
            depth.store = StoreOp::Store;
            depth.clear_depth = 1.0f;
            pass.depth = depth;
            std::unique_ptr<IRenderPassEncoder> shadow_pass = encoder->begin_render_pass(pass);
            shadow_pass->set_pipeline(*shadow_pipe);
            shadow_pass->set_bind_group(0, *shadow_bind);
            shadow_pass->draw(scene_vertex_count(), 1);
            shadow_pass->end();
        }
        {
            RenderPassDesc pass;
            ColorAttachment attach;
            attach.view = color_view.get();
            attach.load = LoadOp::Clear;
            attach.store = StoreOp::Store;
            attach.clear = kClear;
            pass.color.push_back(attach);
            std::unique_ptr<IRenderPassEncoder> main_pass = encoder->begin_render_pass(pass);
            main_pass->set_pipeline(*main_pipe);
            main_pass->set_bind_group(0, *main_bind);
            // Painter order inside one draw: ground triangles first, blocker after — the camera
            // pass needs no depth buffer for this two-layer scene.
            main_pass->draw(scene_vertex_count(), 1);
            main_pass->end();
        }
        TexelCopyBufferLayout layout;
        layout.bytes_per_row = kBytesPerRow;
        layout.rows_per_image = kSize;
        encoder->copy_texture_to_buffer(*color_tex, *readback, layout, {kSize, kSize});

        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device.queue().submit(*commands);

        const auto* pixels = static_cast<const std::uint8_t*>(readback->map_read());
        if (pixels == nullptr)
        {
            std::fprintf(stderr, "[render-lit] FAIL: readback map failed\n");
            return false;
        }
        out.assign(pixels, pixels + static_cast<std::size_t>(kBytesPerRow) * kSize);
        readback->unmap();
        return true;
    };

    auto at = [&](const std::vector<std::uint8_t>& img, PixelCoord p) -> const std::uint8_t*
    { return img.data() + (static_cast<std::size_t>(p.y) * kBytesPerRow) + (p.x * kBpp); };

    int failures = 0;
    auto expect = [&failures](bool ok, const char* what)
    {
        if (!ok)
        {
            std::fprintf(stderr, "[render-lit] MISMATCH: %s\n", what);
            ++failures;
        }
        return ok;
    };

    // GPU pixel ~= CPU reference at a probe point (unorm tolerance covers rasterization + lavapipe
    // float differences vs the CPU mirror).
    constexpr int kTolerance = 14;
    auto match_cpu = [&](const std::vector<std::uint8_t>& img, SamplePoint point,
                         const LitSceneConfig& config, const char* what)
    {
        const Vec3 want = expected_color(point, config);
        const std::uint8_t* px = at(img, sample_pixel(point));
        const bool ok = context::render::detail::pixel_near(px, detail::unorm_byte(want.x),
                                                            detail::unorm_byte(want.y),
                                                            detail::unorm_byte(want.z), 255,
                                                            kTolerance);
        if (!ok)
        {
            std::fprintf(stderr,
                         "[render-lit] MISMATCH: %s — gpu(%u,%u,%u) vs cpu(%d,%d,%d)\n", what,
                         px[0], px[1], px[2], detail::unorm_byte(want.x),
                         detail::unorm_byte(want.y), detail::unorm_byte(want.z));
            ++failures;
        }
    };

    // ---- variants ------------------------------------------------------------------------------
    LitSceneConfig base_cfg;                 // dir + point + shadows on, lightmap off
    LitSceneConfig unlit_cfg;                // every light off — the absent-light path
    unlit_cfg.dir_enabled = false;
    unlit_cfg.point_enabled = false;
    LitSceneConfig dim_cfg = base_cfg;       // light-parameter response: halved sun intensity
    dim_cfg.dir_intensity_scale = 0.5f;
    LitSceneConfig recolor_cfg = base_cfg;   // material-parameter response: blue-ish ground
    recolor_cfg.override_ground_color = true;
    LitSceneConfig no_shadow_cfg = base_cfg; // shadow term isolated
    no_shadow_cfg.shadows_enabled = false;
    LitSceneConfig lightmap_cfg = base_cfg;  // the R-REND-006 hook's trivial sample path
    lightmap_cfg.lightmap_enabled = true;

    std::vector<std::uint8_t> img_base;
    std::vector<std::uint8_t> img_unlit;
    std::vector<std::uint8_t> img_dim;
    std::vector<std::uint8_t> img_recolor;
    std::vector<std::uint8_t> img_no_shadow;
    std::vector<std::uint8_t> img_lightmap;
    if (!render_variant(base_cfg, img_base) || !render_variant(unlit_cfg, img_unlit) ||
        !render_variant(dim_cfg, img_dim) || !render_variant(recolor_cfg, img_recolor) ||
        !render_variant(no_shadow_cfg, img_no_shadow) ||
        !render_variant(lightmap_cfg, img_lightmap))
    {
        return false;
    }

    // (1) Baseline: GPU matches the CPU PBR reference at every probe point.
    match_cpu(img_base, SamplePoint::LitGround, base_cfg, "base/LitGround vs CPU");
    match_cpu(img_base, SamplePoint::ShadowedGround, base_cfg, "base/ShadowedGround vs CPU");
    match_cpu(img_base, SamplePoint::BlockerTop, base_cfg, "base/BlockerTop vs CPU");
    match_cpu(img_base, SamplePoint::PointLitGround, base_cfg, "base/PointLitGround vs CPU");
    match_cpu(img_base, SamplePoint::PointFarGround, base_cfg, "base/PointFarGround vs CPU");

    // The corner outside the ground still shows the clear color.
    expect(context::render::detail::pixel_near(at(img_base, PixelCoord{4, 4}),
                                               detail::unorm_byte(0.02f),
                                               detail::unorm_byte(0.02f),
                                               detail::unorm_byte(0.04f), 255, 6),
           "base/corner is the clear color");

    // (2) AC2 — the shadow occludes: the shadowed probe is much darker than its lit sibling
    // (same material, same normal, no point-light influence at either).
    const int lum_lit = detail::luminance(at(img_base, sample_pixel(SamplePoint::LitGround)));
    const int lum_shadowed =
        detail::luminance(at(img_base, sample_pixel(SamplePoint::ShadowedGround)));
    expect(lum_shadowed + 60 < lum_lit, "shadowed ground is darker than lit ground");

    // ... and with shadows disabled the two probes converge and the shadowed one brightens.
    const int lum_ns_shadowed =
        detail::luminance(at(img_no_shadow, sample_pixel(SamplePoint::ShadowedGround)));
    const int lum_ns_lit =
        detail::luminance(at(img_no_shadow, sample_pixel(SamplePoint::LitGround)));
    expect(lum_ns_shadowed > lum_shadowed + 60, "disabling shadows brightens the shadowed probe");
    expect(lum_ns_shadowed - lum_ns_lit < 2 * kTolerance &&
               lum_ns_lit - lum_ns_shadowed < 2 * kTolerance,
           "without shadows both ground probes match");
    match_cpu(img_no_shadow, SamplePoint::ShadowedGround, no_shadow_cfg,
              "no-shadow/ShadowedGround vs CPU");

    // (3) AC1 — lighting responds to lights being present at all (lit vs unlit) ...
    const int lum_unlit = detail::luminance(at(img_unlit, sample_pixel(SamplePoint::LitGround)));
    expect(lum_unlit + 60 < lum_lit, "unlit render is darker (ambient only)");
    match_cpu(img_unlit, SamplePoint::LitGround, unlit_cfg, "unlit/LitGround vs CPU");

    // ... to a LIGHT parameter (halving the sun intensity dims the open ground) ...
    const int lum_dim = detail::luminance(at(img_dim, sample_pixel(SamplePoint::LitGround)));
    expect(lum_dim + 25 < lum_lit, "halved sun intensity dims the lit ground");
    match_cpu(img_dim, SamplePoint::LitGround, dim_cfg, "dim/LitGround vs CPU");

    // ... to a MATERIAL parameter (blue-ish base color flips the channel ordering) ...
    const std::uint8_t* recolored = at(img_recolor, sample_pixel(SamplePoint::LitGround));
    expect(recolored[2] > recolored[0] + 50, "recolored ground reads blue-dominant");
    match_cpu(img_recolor, SamplePoint::LitGround, recolor_cfg, "recolor/LitGround vs CPU");

    // ... and to point-light distance (falloff: under the lamp vs beyond its range).
    const int lum_point_near =
        detail::luminance(at(img_base, sample_pixel(SamplePoint::PointLitGround)));
    const int lum_point_far =
        detail::luminance(at(img_base, sample_pixel(SamplePoint::PointFarGround)));
    expect(lum_point_near > lum_point_far + 30, "point light falls off with distance");

    // (4) AC3 — the lightmap INPUT hook round-trips: enabling it shifts a ground probe by exactly
    // lightmap * albedo (per channel), and leaves the hook-free blocker untouched.
    {
        const std::uint8_t* with_lm = at(img_lightmap, sample_pixel(SamplePoint::LitGround));
        const std::uint8_t* without = at(img_base, sample_pixel(SamplePoint::LitGround));
        const Vec3 want_with = expected_color(SamplePoint::LitGround, lightmap_cfg);
        const Vec3 want_without = expected_color(SamplePoint::LitGround, base_cfg);
        const int want_delta[3] = {
            detail::unorm_byte(want_with.x) - detail::unorm_byte(want_without.x),
            detail::unorm_byte(want_with.y) - detail::unorm_byte(want_without.y),
            detail::unorm_byte(want_with.z) - detail::unorm_byte(want_without.z)};
        bool delta_ok = true;
        for (int c = 0; c < 3; ++c)
        {
            const int got = static_cast<int>(with_lm[c]) - static_cast<int>(without[c]);
            if (got < want_delta[c] - 10 || got > want_delta[c] + 10)
            {
                delta_ok = false;
            }
        }
        expect(delta_ok, "lightmap hook adds lightmap*albedo on the ground probe");

        const std::uint8_t* blocker_with = at(img_lightmap, sample_pixel(SamplePoint::BlockerTop));
        const std::uint8_t* blocker_without = at(img_base, sample_pixel(SamplePoint::BlockerTop));
        bool blocker_same = true;
        for (int c = 0; c < 3; ++c)
        {
            const int diff = static_cast<int>(blocker_with[c]) - blocker_without[c];
            if (diff < -6 || diff > 6)
            {
                blocker_same = false;
            }
        }
        expect(blocker_same, "hook-free blocker is unaffected by the lightmap toggle");
        match_cpu(img_lightmap, SamplePoint::LitGround, lightmap_cfg, "lightmap/LitGround vs CPU");
    }

    std::printf("[render-lit] probes: lit=%d shadowed=%d unlit=%d dim=%d point_near=%d "
                "point_far=%d no_shadow=%d/%d\n",
                lum_lit, lum_shadowed, lum_unlit, lum_dim, lum_point_near, lum_point_far,
                lum_ns_shadowed, lum_ns_lit);

    if (failures == 0)
    {
        std::printf("[render-lit] PASS\n");
        return true;
    }
    std::fprintf(stderr, "[render-lit] FAIL: %d assertion(s) did not match\n", failures);
    return false;
}

} // namespace context::render::lit
