// The `ui-hud` golden corpus scene (M7 a6, R-UI-005; issue #141 golden discipline). A reference HUD of
// solid colored rectangles — a health bar (background + fill), a minimap panel, a GPU-composited
// (transformed) badge, and a status bar — authored as a retained UiTree, extracted through the UI
// backend, and repainted into the GpuUiProvider's persistent layer. The layer read-back IS the golden
// frame, so the committed goldens/ui-hud.ppm can never drift from what the provider actually renders
// (the same "the golden is the proof's frame by construction" discipline as triangle3d/sprite2d/viewport).
//
// KERNEL-FREE (like golden.h / viewport_scene.h): the HUD drives only context_ui's retained tree + the
// RHI + the pure-CPU ortho quad math, so this header compiles into BOTH the native offscreen exe and the
// Emscripten web target (the ui-hud golden's web leg). Text/glyphs are deliberately absent — they arrive
// with a7/a8; a6 pins the colored-rect HUD.
//
// Two render paths share the ONE reference scene + extract:
//   * render_golden_ui_hud / render_offscreen_ui_hud — the GPU path (real adapter: lavapipe native +
//     SwiftShader web), the SSIM-gated golden + the CI readback assert;
//   * render_ui_hud_reference_cpu — the analytic, GPU-free rasterization of the SAME extracted quads
//     (opaque axis-aligned fills, painter order), which generates the committed baseline and is asserted
//     locally on the fake backend (the CPU can't drive the GPU quad path, exactly as sprite2d).

#pragma once

#include "context/packages/ui/provider.h" // RepaintPlan
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/golden.h" // golden::GoldenImage + write_ppm
#include "context/render/rhi.h"
#include "context/render/ui/provider.h"
#include "context/render/ui/snapshot.h"

#include <cstdint>
#include <cstdio>
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

// Render the reference HUD offscreen through `device` into `out` — the golden dump path. Drives the
// GpuUiProvider (full repaint into its persistent layer) and reads the layer back. Returns false only
// when the readback map fails. `device` must be a live (GPU or fake) device.
inline bool render_golden_ui_hud(IDevice& device, golden::GoldenImage& out)
{
    const std::uint32_t size = ui_hud_target_size();
    GpuUiProvider provider(device, Extent2D{size, size}, ui_hud_clear_color());
    packages::ui::UiTree tree;
    build_reference_hud(tree);
    packages::ui::RepaintPlan plan;
    plan.full_repaint = true;
    provider.present(tree, plan);
    out.width = size;
    out.height = size;
    return provider.read_layer(out.rgba);
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
