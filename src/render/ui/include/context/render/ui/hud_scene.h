// The `ui-hud` golden corpus scene (M7 a6, R-UI-005; issue #141 golden discipline). A reference HUD of
// solid colored rectangles — a health bar (background + fill), a minimap panel, a GPU-composited
// (transformed) badge, and a status bar — authored as a retained UiTree, extracted through the UI
// backend, and repainted into the GpuUiProvider's persistent layer. The layer read-back IS the golden
// frame, so the committed goldens/ui-hud.ppm can never drift from what the provider actually renders
// (the same "the golden is the proof's frame by construction" discipline as triangle3d/sprite2d/viewport).
//
// KERNEL-FREE (like golden.h / viewport_scene.h): the HUD drives only context_ui's retained tree + the
// RHI + the pure-CPU ortho quad math. M7 a8 adds a SHAPED-TEXT label (measure -> glyph-id atlas ->
// atlas-textured glyph draw), so this header now also links context_ui_text (FreeType + HarfBuzz) — still
// GPU-abstraction-only (drives rhi.h), so it compiles into BOTH the native offscreen exe and the
// Emscripten web target (the ui-hud golden's web leg). The text is drawn as a CUTOUT (the atlas coverage
// thresholded at 0.5) with 1:1 integer pixel<->texel placement, so the GPU (nearest sample + discard) and
// the analytic CPU mirror produce the SAME 1-bit glyph mask — a backend-stable golden (the T1 RHI has no
// alpha blend; a cutout needs none). Complex-script shaping CORRECTNESS is proven by the headless
// ui-test_shaping suite; the golden pins that shaped text reaches pixels on GPU + web.
//
// Two render paths share the ONE reference scene + extract:
//   * render_golden_ui_hud / render_offscreen_ui_hud — the GPU path (real adapter: lavapipe native +
//     SwiftShader web), the SSIM-gated golden + the CI readback assert;
//   * render_ui_hud_reference_cpu — the analytic, GPU-free rasterization of the SAME extracted quads
//     (opaque axis-aligned fills, painter order), which generates the committed baseline and is asserted
//     locally on the fake backend (the CPU can't drive the GPU quad path, exactly as sprite2d).

#pragma once

#include "context/packages/ui/provider.h" // RepaintPlan
#include "context/packages/ui/text/embedded_fonts.h" // M7 a8: shaped text label
#include "context/packages/ui/text/font.h"
#include "context/packages/ui/text/measure.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/golden.h" // golden::GoldenImage + write_ppm
#include "context/render/rhi.h"
#include "context/render/sprite/ortho.h"       // M7 a8: glyph-quad clip corners (same ortho as the rects)
#include "context/render/ui/glyph_atlas.h"     // M7 a8: glyph-id-keyed atlas
#include "context/render/ui/provider.h"
#include "context/render/ui/snapshot.h"
#include "context/render/ui/text_quads.h"      // M7 a8: offset-bearing atlas quad emitter

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::render::ui
{

// The HUD golden's target edge (square RGBA8), shared by the GPU render, the analytic baseline, and the
// committed goldens/ui-hud.ppm. 256 like every other corpus scene.
[[nodiscard]] constexpr std::uint32_t ui_hud_target_size()
{
    return 256;
}

// The surface background the HUD composites over — the stand-in for the composited 3D-pass output
// behind the UI at this offscreen tier. A dark slate, distinct from the sprite/triangle corpus clear.
[[nodiscard]] inline Color ui_hud_clear_color()
{
    return Color{0.06, 0.07, 0.10, 1.0};
}

// Build the reference HUD into `tree` (which starts with only its Root). Every node carries an explicit
// `bounds` (surface pixels, y-down top-left) + an opaque background; the health FILL is a child of the
// bar (so it paints on top), and the badge carries a composite transform (scale about centre + translate)
// to exercise composited_transforms. Pre-order paint: bar-bg, bar-fill, minimap, badge, status.
inline void build_reference_hud(packages::ui::UiTree& tree)
{
    using packages::ui::Color;
    using packages::ui::NodeId;
    using packages::ui::Rect;
    using packages::ui::Role;
    using packages::ui::Style;

    auto panel = [&tree](NodeId parent, Role role, const Rect& bounds, const Color& bg) -> NodeId
    {
        const NodeId id = tree.create_node(role, parent);
        tree.set_bounds(id, bounds);
        Style style;
        style.background = bg;
        tree.set_style(id, style);
        return id;
    };

    const NodeId root = tree.root();
    // Health bar: dark background with a green fill child (fill paints ON TOP of the background).
    const NodeId bar = panel(root, Role::Panel, Rect{16, 16, 120, 20}, Color{40, 40, 48, 255});
    panel(bar, Role::ProgressBar, Rect{18, 18, 80, 16}, Color{60, 200, 90, 255});
    // Minimap panel (bottom-right), blue.
    panel(root, Role::Image, Rect{176, 176, 64, 64}, Color{40, 80, 160, 255});
    // GPU-composited badge (top-right), amber — a scale-about-centre + translate applied at composite
    // time (no relayout): base bounds (200,20,32,32) -> transformed (192,20,40,40).
    {
        const NodeId badge = tree.create_node(Role::Label, root);
        tree.set_bounds(badge, Rect{200, 20, 32, 32});
        Style style;
        style.background = Color{220, 180, 40, 255};
        style.transform.scale = {1.25f, 1.25f};
        style.transform.translate = {-4.0f, 4.0f};
        tree.set_style(badge, style);
    }
    // Status bar along the bottom, near-black.
    panel(root, Role::Panel, Rect{16, 224, 224, 16}, Color{30, 30, 36, 255});
}

// ------------------------------------------------------------------------------- M7 a8 shaped text

// The HUD's shaped-text label: a short Latin string that drives the full measure -> shape -> glyph-id
// atlas -> textured-glyph-draw path into the golden. Placed in the HUD's empty mid-left region (clear of
// every colored-rect probe). White so it never collides with a panel color.
[[nodiscard]] inline const char* ui_hud_label()
{
    return "SCORE 1200";
}
[[nodiscard]] constexpr float ui_hud_label_px()
{
    return 20.0f;
}
[[nodiscard]] constexpr float ui_hud_label_pen_x()
{
    return 20.0f;
}
[[nodiscard]] constexpr float ui_hud_label_pen_y()
{
    return 110.0f; // baseline, y-down
}
[[nodiscard]] inline packages::ui::Color ui_hud_label_color()
{
    return packages::ui::Color{255, 255, 255, 255};
}

// The shaped label as an atlas (R8 coverage) + INTEGER-snapped screen glyph quads. Integer x/y + quad
// size == atlas slot size gives a 1:1 pixel<->texel map, so the GPU (nearest sample + cutout) and the CPU
// mirror (direct threshold) draw the SAME 1-bit glyph mask. Empty if the font/atlas is unavailable.
struct HudText
{
    std::vector<std::uint8_t> atlas_r8; // atlas coverage buffer (atlas_w * atlas_h), row-major
    int atlas_w = 0;
    int atlas_h = 0;
    std::vector<TextQuad> quads; // integer-snapped screen rects + atlas UVs (visual order)
    std::uint32_t px = 0;
};

inline void build_hud_text(HudText& out)
{
    namespace tx = packages::ui::text;
    auto font = tx::FontFace::from_memory(tx::noto_sans_regular());
    if (!font)
        return;
    out.px = static_cast<std::uint32_t>(ui_hud_label_px());
    GlyphAtlas atlas(GlyphAtlas::Config{256, 256, 64, 1});
    GlyphRasterizer raster = [&](const AtlasKey& k) -> std::optional<GlyphCoverage>
    {
        auto b = font->rasterize(k.glyph, static_cast<float>(k.px));
        if (!b)
            return std::nullopt;
        GlyphCoverage c;
        c.width = b->width;
        c.height = b->height;
        c.left = b->left;
        c.top = b->top;
        c.advance = b->advance;
        c.pixels = std::move(b->coverage);
        return c;
    };
    const std::vector<tx::ShapedRun> runs = tx::measure(*font, ui_hud_label(), ui_hud_label_px());
    float pen_x = ui_hud_label_pen_x();
    const float pen_y = ui_hud_label_pen_y();
    for (const tx::ShapedRun& run : runs)
    {
        std::vector<PositionedGlyph> pg;
        pg.reserve(run.glyphs.size());
        for (const tx::GlyphMetrics& g : run.glyphs)
            pg.push_back(PositionedGlyph{g.glyph, g.advance, g.x_offset, g.y_offset});
        std::vector<TextQuad> quads =
            emit_text_quads(pg, run.font, out.px, pen_x, pen_y, atlas, raster, ui_hud_label_color());
        for (TextQuad q : quads)
        {
            // Snap the origin to integer pixels; keep the (integer) atlas-slot size, so the whole rect is
            // integer-aligned and each screen pixel maps to exactly one atlas texel.
            const float w = q.x1 - q.x0;
            const float h = q.y1 - q.y0;
            q.x0 = std::round(q.x0);
            q.y0 = std::round(q.y0);
            q.x1 = q.x0 + w;
            q.y1 = q.y0 + h;
            out.quads.push_back(q);
        }
        pen_x += run.width;
    }
    out.atlas_w = atlas.width();
    out.atlas_h = atlas.height();
    out.atlas_r8 = atlas.pixels();
}

// A per-glyph textured-CUTOUT WGSL shader: two triangles from the four clip corners (bl,br,tr,tl —
// quad_clip_corners order) with the glyph's atlas UVs; samples the R8-as-RGBA coverage, DISCARDS below
// 0.5 (no blend needed — the T1 RHI has none), and outputs the flat text color where covered. Mirrors
// worldpanel_textured_quad_wgsl's geometry; the atlas top texel (v0) maps to the top clip corners.
[[nodiscard]] inline std::string ui_hud_glyph_wgsl(const std::array<sprite::Vec2, 4>& c, float u0,
                                                   float v0, float u1, float v1, const float color[4])
{
    std::ostringstream w;
    w.imbue(std::locale::classic()); // force '.' decimals — a non-classic locale would emit invalid WGSL
    w.setf(std::ios::fixed);
    w.precision(6);
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
      << "        vec2f(" << u0 << ", " << v1 << "), vec2f(" << u1 << ", " << v1 << "), vec2f(" << u1
      << ", " << v0 << "),\n"
      << "        vec2f(" << u0 << ", " << v1 << "), vec2f(" << u1 << ", " << v0 << "), vec2f(" << u0
      << ", " << v0 << "));\n"
      << "    var o : VsOut;\n"
      << "    o.pos = p[i];\n"
      << "    o.uv = uv[i];\n"
      << "    return o;\n"
      << "}\n\n"
      << "@fragment\n"
      << "fn fs_main(in : VsOut) -> @location(0) vec4f {\n"
      << "    let cov = textureSample(tex, samp, in.uv).r;\n"
      << "    if (cov < 0.5) { discard; }\n"
      << "    return vec4f(" << color[0] << ", " << color[1] << ", " << color[2] << ", " << color[3]
      << ");\n"
      << "}\n";
    return w.str();
}

// Draw the shaped label into `target` on top of the already-painted rects (LoadOp::Load). Uploads the
// glyph atlas once (R8 coverage expanded to RGBA), then draws each glyph as a cutout quad sampling it. A
// no-op on the fake backend (its textured passes don't rasterize) — the CPU mirror is the local oracle.
inline void draw_hud_text_gpu(IDevice& device, ITexture& target, std::uint32_t size)
{
    HudText t;
    build_hud_text(t);
    if (t.quads.empty() || t.atlas_w <= 0 || t.atlas_h <= 0)
        return;

    // Upload the atlas as RGBA8 (coverage replicated into every channel; the shader samples .r).
    TextureDesc atd;
    atd.size = {static_cast<std::uint32_t>(t.atlas_w), static_cast<std::uint32_t>(t.atlas_h)};
    atd.format = TextureFormat::RGBA8Unorm;
    atd.copy_dst = true;
    atd.texture_binding = true;
    std::unique_ptr<ITexture> atlas_tex = device.create_texture(atd);
    std::vector<std::uint8_t> rgba(t.atlas_r8.size() * 4u, 0u);
    for (std::size_t i = 0; i < t.atlas_r8.size(); ++i)
    {
        const std::uint8_t cov = t.atlas_r8[i];
        rgba[i * 4u + 0] = cov;
        rgba[i * 4u + 1] = cov;
        rgba[i * 4u + 2] = cov;
        rgba[i * 4u + 3] = cov;
    }
    TexelCopyBufferLayout lay;
    lay.bytes_per_row = static_cast<std::uint32_t>(t.atlas_w) * 4u; // 256*4 = 1024, 256-aligned
    lay.rows_per_image = static_cast<std::uint32_t>(t.atlas_h);
    device.queue().write_texture(*atlas_tex, rgba.data(), rgba.size(), lay,
                                 {static_cast<std::uint32_t>(t.atlas_w),
                                  static_cast<std::uint32_t>(t.atlas_h)});
    std::unique_ptr<ITextureView> atlas_view = atlas_tex->create_view();
    SamplerDesc samp_desc; // nearest/clamp default — crisp texels, backend-stable
    std::unique_ptr<ISampler> sampler = device.create_sampler(samp_desc);

    // The SAME ortho projection the GpuUiProvider uses for the rects (surface pixels y-down -> clip).
    sprite::Camera2D cam;
    cam.center = {static_cast<float>(size) * 0.5f, static_cast<float>(size) * 0.5f};
    cam.half_width = static_cast<float>(size) * 0.5f;
    cam.half_height = static_cast<float>(size) * 0.5f;
    const sprite::Mat4 proj = cam.projection();
    std::unique_ptr<ITextureView> target_view = target.create_view();

    for (const TextQuad& q : t.quads)
    {
        const float cx = (q.x0 + q.x1) * 0.5f;
        const float cy = static_cast<float>(size) - (q.y0 + q.y1) * 0.5f; // flip y (like provider)
        const std::array<sprite::Vec2, 4> corners =
            sprite::quad_clip_corners(proj, sprite::Vec2{cx, cy}, sprite::Vec2{q.x1 - q.x0, q.y1 - q.y0});
        const float color[4] = {static_cast<float>(q.color.r) / 255.0f,
                                static_cast<float>(q.color.g) / 255.0f,
                                static_cast<float>(q.color.b) / 255.0f, 1.0f};
        RenderPipelineDesc pipe_desc;
        pipe_desc.wgsl = ui_hud_glyph_wgsl(corners, q.u0, q.v0, q.u1, q.v1, color);
        pipe_desc.color_format = TextureFormat::RGBA8Unorm;
        std::unique_ptr<IRenderPipeline> pipeline = device.create_render_pipeline(pipe_desc);
        std::unique_ptr<IBindGroupLayout> layout = pipeline->bind_group_layout(0);
        std::vector<BindGroupEntry> entries(2);
        entries[0].binding = 0;
        entries[0].texture = atlas_view.get();
        entries[1].binding = 1;
        entries[1].sampler = sampler.get();
        std::unique_ptr<IBindGroup> bind = device.create_bind_group(*layout, entries);

        std::unique_ptr<ICommandEncoder> encoder = device.create_command_encoder();
        RenderPassDesc pass_desc;
        ColorAttachment attach;
        attach.view = target_view.get();
        attach.load = LoadOp::Load; // composite on top of the rects
        attach.store = StoreOp::Store;
        attach.clear = ui_hud_clear_color();
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
}

// Render the reference HUD offscreen through `device` into `out` — the golden dump path. Owns the target
// texture (so the shaped text composites on top of the provider's rects), drives the GpuUiProvider full
// repaint, draws the text, and reads the target back. Returns false only when the readback map fails.
// `device` must be a live (GPU or fake) device.
inline bool render_golden_ui_hud(IDevice& device, golden::GoldenImage& out)
{
    const std::uint32_t size = ui_hud_target_size();
    TextureDesc td;
    td.size = {size, size};
    td.format = TextureFormat::RGBA8Unorm;
    td.render_attachment = true;
    td.copy_src = true; // read back for the golden dump / CI proof
    std::unique_ptr<ITexture> target = device.create_texture(td);

    GpuUiProvider provider(device, *target, Extent2D{size, size}, ui_hud_clear_color());
    packages::ui::UiTree tree;
    build_reference_hud(tree);
    packages::ui::RepaintPlan plan;
    plan.full_repaint = true;
    provider.present(tree, plan);       // the colored rects
    draw_hud_text_gpu(device, *target, size); // the shaped text on top (cutout)

    out.width = size;
    out.height = size;
    return provider.read_layer(out.rgba); // read_layer reads the external target (rects + text)
}

// Analytic (GPU-free) rasterization of the SAME extracted HUD quads: fill the clear, then paint each
// quad's pixel-covered box (pixel-centre-inside, exactly the GPU's coverage rule for an axis-aligned
// quad) in painter order. This IS the committed baseline (generated locally, no GPU) and the fake-backend
// pixel oracle — the GPU render (native/web) SSIM-matches it. Rows top-first, tight RGBA8.
inline void render_ui_hud_reference_cpu(golden::GoldenImage& out)
{
    const std::uint32_t size = ui_hud_target_size();
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4u, 0u);

    auto unorm = [](double c) -> std::uint8_t
    {
        if (c < 0.0)
            c = 0.0;
        if (c > 1.0)
            c = 1.0;
        return static_cast<std::uint8_t>(c * 255.0 + 0.5);
    };
    const Color clear = ui_hud_clear_color();
    const std::uint8_t cr = unorm(clear.r), cg = unorm(clear.g), cb = unorm(clear.b),
                       ca = unorm(clear.a);
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4)
    {
        out.rgba[i + 0] = cr;
        out.rgba[i + 1] = cg;
        out.rgba[i + 2] = cb;
        out.rgba[i + 3] = ca;
    }

    packages::ui::UiTree tree;
    build_reference_hud(tree);
    UiRenderSnapshot snap;
    extract_ui(tree, packages::ui::Rect{0, 0, static_cast<float>(size), static_cast<float>(size)},
               snap);
    for (const UiQuad& quad : snap.quads) // pre-order == painter order; later overdraws earlier
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
                std::uint8_t* p = out.rgba.data() + (static_cast<std::size_t>(y) * size + x) * 4u;
                p[0] = quad.color.r;
                p[1] = quad.color.g;
                p[2] = quad.color.b;
                p[3] = 255;
            }
        }
    }

    // Shaped text label (M7 a8): place each glyph 1:1 from the atlas coverage (threshold at 0.5 -> the
    // SAME 1-bit mask the GPU cutout shader draws). Integer rects + slot-sized quads => one atlas texel
    // per screen pixel, so this analytic mirror equals the GPU (lavapipe / SwiftShader) render.
    HudText t;
    build_hud_text(t);
    for (const TextQuad& q : t.quads)
    {
        const int x0 = static_cast<int>(std::lround(q.x0));
        const int y0 = static_cast<int>(std::lround(q.y0));
        const int sw = static_cast<int>(std::lround(q.x1 - q.x0));
        const int sh = static_cast<int>(std::lround(q.y1 - q.y0));
        const int ax0 = static_cast<int>(std::lround(q.u0 * static_cast<float>(t.atlas_w)));
        const int ay0 = static_cast<int>(std::lround(q.v0 * static_cast<float>(t.atlas_h)));
        for (int dy = 0; dy < sh; ++dy)
        {
            for (int dx = 0; dx < sw; ++dx)
            {
                const int sx = x0 + dx;
                const int sy = y0 + dy;
                if (sx < 0 || sx >= static_cast<int>(size) || sy < 0 || sy >= static_cast<int>(size))
                    continue;
                const int ax = ax0 + dx;
                const int ay = ay0 + dy;
                if (ax < 0 || ax >= t.atlas_w || ay < 0 || ay >= t.atlas_h)
                    continue;
                const std::uint8_t cov =
                    t.atlas_r8[static_cast<std::size_t>(ay) * t.atlas_w + static_cast<std::size_t>(ax)];
                if (cov >= 128u) // cutout at 0.5 (matches the WGSL `cov < 0.5 -> discard`)
                {
                    std::uint8_t* p =
                        out.rgba.data() + (static_cast<std::size_t>(sy) * size + sx) * 4u;
                    p[0] = q.color.r;
                    p[1] = q.color.g;
                    p[2] = q.color.b;
                    p[3] = 255u;
                }
            }
        }
    }
}

// Render the HUD through `device` and assert the read-back proves the composite: the clear where no
// panel covers, a solid panel's color, the health FILL winning over its background in the overlap, and
// the composited (transformed) badge at its post-transform position. `device` must actually rasterize
// (the wgpu backend on CI); the GPU-free fake backend rasterizes only 3-vertex draws, so the quad
// passes are no-ops there — this assert is the CI (lavapipe) proof, the fake-backend coverage is the
// analytic oracle above. Returns true on a passing readback.
inline bool render_offscreen_ui_hud(IDevice& device)
{
    golden::GoldenImage img;
    if (!render_golden_ui_hud(device, img))
    {
        std::fprintf(stderr, "[render-ui-hud] FAIL: layer readback failed\n");
        return false;
    }
    const std::uint32_t bpr = ui_hud_target_size() * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    // NOTE: not named `near` — <windows.h> (pulled by the wgpu backend on the MSVC leg) macro-defines
    // `near`/`far`, which would mangle the identifier.
    auto near_rgb = [](const std::uint8_t* px, int r, int g, int b, int tol)
    {
        auto ok = [tol](int a, int e) { return a - e >= -tol && a - e <= tol; };
        return ok(px[0], r) && ok(px[1], g) && ok(px[2], b);
    };

    const bool bg_ok = near_rgb(at(4, 4), 15, 18, 26, 8);         // clear corner (no panel)
    const bool map_ok = near_rgb(at(208, 208), 40, 80, 160, 6);  // minimap panel, solid blue
    const bool fill_ok = near_rgb(at(50, 25), 60, 200, 90, 6);   // health fill OVER its background
    const bool badge_ok = near_rgb(at(210, 40), 220, 180, 40, 6); // composited (transformed) badge

    std::printf("[render-ui-hud] bg(4,4)=%s minimap(208,208)=%s fill(50,25)=%s badge(210,40)=%s\n",
                bg_ok ? "ok" : "MISMATCH", map_ok ? "ok" : "MISMATCH", fill_ok ? "ok/fill-on-top" : "MISMATCH",
                badge_ok ? "ok/composited" : "MISMATCH");

    const bool ok = bg_ok && map_ok && fill_ok && badge_ok;
    if (ok)
    {
        std::printf("[render-ui-hud] PASS\n");
    }
    else
    {
        std::fprintf(stderr, "[render-ui-hud] FAIL: HUD readback did not match (bg=%d map=%d fill=%d "
                             "badge=%d)\n",
                     bg_ok, map_ok, fill_ok, badge_ok);
    }
    return ok;
}

} // namespace context::render::ui
