// The per-window compositor (03 §4): the extrapolated layer UV, the premultiplied CPU blend, the
// layer ORDER, damage-driven redraw, the resize protocol, the PET_POPUP second layer AND its
// DIP -> physical destination at a non-integral scale (a2), the Outdated/Lost/Suboptimal acquire
// branches, and both present paths.
//
// The GPU half is driven through render_test_rhi.h's fake backend — the same GPU-free fake the e03
// present tests use — so the real composite path (pipeline, per-layer bind groups, scissors, submit,
// present) runs on all three OS legs with no adapter anywhere.

#include "context/editor/shell/compositor.h"

#include "render_test_rhi.h"
#include "shell_test.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;
namespace present = context::render::present;

namespace
{

// A producer frame whose ALLOCATION is larger than its visible rect — the shape that catches the UV
// bug e03 documents (sampling [0,1] stretches the unused margin across the window).
BrowserFrame make_view_frame(std::vector<std::uint8_t>& storage, render::Extent2D coded,
                             const render::Rect2D& visible, std::uint8_t b, std::uint8_t g,
                             std::uint8_t r, std::uint8_t a)
{
    storage = make_premultiplied_bgra(coded, coded.width * 4u, b, g, r, a);
    BrowserFrame frame;
    frame.layer = BrowserLayer::view;
    frame.frame.pixels = storage.data();
    frame.frame.byte_size = storage.size();
    frame.frame.bytes_per_row = coded.width * 4u;
    frame.frame.coded_size = coded;
    frame.frame.visible_rect = visible;
    return frame;
}

CompositorConfig software_config()
{
    CompositorConfig config;
    // force_software is the L-41 switch: the degrade is a decision, never an accident. The Session-0
    // smoke and these tests both take the software path deliberately.
    config.import_options.force_software = true;
    return config;
}

// ------------------------------------------------------------------------------ compute_layer_uv

void test_layer_uv_full_window_is_the_identity()
{
    const render::Extent2D coded{1024, 1024};
    const render::Rect2D visible = shelltest::rect(0, 0, 800, 600);
    const render::Extent2D target{800, 600};

    const present::CompositeUv want = present::compute_composite_uv(visible, coded);
    const present::CompositeUv got =
        compute_layer_uv(render::Rect2D{render::Origin2D{}, target}, target, visible, coded);

    // The full-window case MUST reduce to e03's own function — otherwise the two would drift and the
    // main CEF layer would sample differently from the offscreen proof that pins the arithmetic.
    CHECK(shelltest::near_eq(got.u0, want.u0));
    CHECK(shelltest::near_eq(got.v0, want.v0));
    CHECK(shelltest::near_eq(got.u1, want.u1));
    CHECK(shelltest::near_eq(got.v1, want.v1));
}

void test_layer_uv_extrapolates_so_a_sub_rect_samples_correctly()
{
    // A popup at (100,50) sized 200x100 inside an 800x600 window, sourced from a 256x256 allocation
    // whose visible rect is the whole thing.
    const render::Extent2D coded{256, 256};
    const render::Rect2D visible = shelltest::rect(0, 0, 256, 256);
    const render::Extent2D target{800, 600};
    const render::Rect2D dst = shelltest::rect(100, 50, 200, 100);

    const present::CompositeUv uv = compute_layer_uv(dst, target, visible, coded);

    // The property that matters: interpolating the returned UV linearly across the WHOLE target must
    // land on [0,1] exactly at the destination rect's edges. Assert it by evaluating the
    // interpolation the vertex stage performs.
    auto u_at = [&](float x) { return uv.u0 + (uv.u1 - uv.u0) * (x / 800.0f); };
    auto v_at = [&](float y) { return uv.v0 + (uv.v1 - uv.v0) * (y / 600.0f); };

    CHECK(shelltest::near_eq(u_at(100.0f), 0.0f));
    CHECK(shelltest::near_eq(u_at(300.0f), 1.0f)); // 100 + 200
    CHECK(shelltest::near_eq(v_at(50.0f), 0.0f));
    CHECK(shelltest::near_eq(v_at(150.0f), 1.0f)); // 50 + 100

    // Halfway across the popup samples the middle of the source.
    CHECK(shelltest::near_eq(u_at(200.0f), 0.5f));

    // Without the extrapolation the popup would sample whatever [0,1]-over-the-window happens to
    // put at its corner — assert the returned UV is NOT the naive one.
    CHECK(!shelltest::near_eq(uv.u0, 0.0f));
}

void test_layer_uv_honours_a_visible_sub_rect()
{
    // A visible rect that is a SUB-rect of the allocation (the common CEF case) still maps onto the
    // destination rect exactly.
    const render::Extent2D coded{512, 512};
    const render::Rect2D visible = shelltest::rect(0, 0, 400, 300);
    const render::Extent2D target{400, 300};
    const render::Rect2D dst = shelltest::rect(0, 0, 400, 300);

    const present::CompositeUv uv = compute_layer_uv(dst, target, visible, coded);
    CHECK(shelltest::near_eq(uv.u0, 0.0f));
    CHECK(shelltest::near_eq(uv.u1, 400.0f / 512.0f)); // visible/coded, not 1.0
    CHECK(shelltest::near_eq(uv.v1, 300.0f / 512.0f));
}

void test_layer_uv_degenerate_inputs_fall_back_rather_than_divide_by_zero()
{
    const render::Extent2D coded{256, 256};
    const render::Rect2D visible = shelltest::rect(0, 0, 256, 256);
    // An empty destination rect or target: return e03's answer rather than dividing by zero.
    const present::CompositeUv empty_dst =
        compute_layer_uv(shelltest::rect(0, 0, 0, 0), render::Extent2D{800, 600}, visible, coded);
    CHECK(shelltest::near_eq(empty_dst.u1, 1.0f));
    const present::CompositeUv empty_target =
        compute_layer_uv(shelltest::rect(0, 0, 10, 10), render::Extent2D{0, 0}, visible, coded);
    CHECK(shelltest::near_eq(empty_target.u1, 1.0f));
}

// --------------------------------------------------------------------- blend_premultiplied_bgra

void test_premultiplied_blend()
{
    const render::Extent2D dst_size{4, 2};
    std::vector<std::uint8_t> dst =
        make_premultiplied_bgra(dst_size, dst_size.width * 4u, 10, 20, 30, 255);

    // OPAQUE source replaces.
    std::vector<std::uint8_t> opaque =
        make_premultiplied_bgra(render::Extent2D{2, 1}, 8, 200, 100, 50, 255);
    blend_premultiplied_bgra(dst.data(), dst_size, dst_size.width * 4u, opaque.data(),
                             render::Extent2D{2, 1}, 8, render::Origin2D{0, 0});
    CHECK(dst[0] == 200);
    CHECK(dst[1] == 100);
    CHECK(dst[2] == 50);
    CHECK(dst[3] == 255);
    // ...and leaves the rest alone.
    CHECK(dst[8] == 10);

    // FULLY TRANSPARENT source is a no-op (premultiplied: colour is already 0).
    std::vector<std::uint8_t> clear =
        make_premultiplied_bgra(render::Extent2D{1, 1}, 4, 0, 0, 0, 0);
    const std::uint8_t before = dst[8];
    blend_premultiplied_bgra(dst.data(), dst_size, dst_size.width * 4u, clear.data(),
                             render::Extent2D{1, 1}, 4, render::Origin2D{2, 0});
    CHECK(dst[8] == before);

    // HALF alpha, premultiplied: dst = src + dst*(1-a). The source colour is ALREADY multiplied by
    // alpha, so it is added rather than lerped — using SRC_ALPHA/INV_SRC_ALPHA here would
    // double-darken every antialiased edge in the UI.
    std::vector<std::uint8_t> half =
        make_premultiplied_bgra(render::Extent2D{1, 1}, 4, 60, 60, 60, 128);
    blend_premultiplied_bgra(dst.data(), dst_size, dst_size.width * 4u, half.data(),
                             render::Extent2D{1, 1}, 4, render::Origin2D{3, 0});
    // 60 + 10*(127/255) ~= 65
    CHECK(dst[12] >= 64 && dst[12] <= 66);
}

void test_premultiplied_blend_clips_at_the_destination_edges()
{
    const render::Extent2D dst_size{4, 2};
    std::vector<std::uint8_t> dst =
        make_premultiplied_bgra(dst_size, dst_size.width * 4u, 0, 0, 0, 255);
    std::vector<std::uint8_t> src =
        make_premultiplied_bgra(render::Extent2D{4, 4}, 16, 255, 255, 255, 255);

    // A source larger than the space left at the origin is CLIPPED, not written out of bounds.
    blend_premultiplied_bgra(dst.data(), dst_size, dst_size.width * 4u, src.data(),
                             render::Extent2D{4, 4}, 16, render::Origin2D{2, 1});
    CHECK(dst[dst.size() - 1] == 255); // the last texel was written
    CHECK(dst[0] == 0);                // untouched

    // An origin fully outside is a no-op rather than a crash, and so is a null buffer.
    blend_premultiplied_bgra(dst.data(), dst_size, dst_size.width * 4u, src.data(),
                             render::Extent2D{4, 4}, 16, render::Origin2D{99, 99});
    blend_premultiplied_bgra(nullptr, dst_size, 16, src.data(), render::Extent2D{4, 4}, 16,
                             render::Origin2D{});
}

// ------------------------------------------------------------------------------- the GPU path

void test_damage_driven_redraw_skips_an_undamaged_frame()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    CHECK(compositor.path() == PresentPath::gpu_swapchain);

    // A freshly attached window has never drawn, so the first frame is damaged deliberately.
    CHECK(compositor.damage().any());
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 1);

    // Nothing changed: the frame is SKIPPED, not re-presented. A shell that presented
    // unconditionally would burn a queue submit per vsync on a completely static editor.
    CHECK(!compositor.render_frame());
    CHECK(!compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 1);
    CHECK(compositor.stats().frames_skipped_no_damage == 2);

    // A browser paint damages it again.
    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_view_frame(storage, render::Extent2D{1024, 1024},
                                                shelltest::rect(0, 0, 800, 600), 10, 20, 30, 255));
    CHECK(compositor.damage().browser_paint);
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 2);
}

void test_layer_order_and_the_popup_scissor()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));

    // One viewport slot with content.
    render::TextureDesc rt_desc;
    rt_desc.size = render::Extent2D{400, 300};
    rt_desc.render_attachment = true;
    std::unique_ptr<render::ITexture> rt = device.create_texture(rt_desc);
    std::unique_ptr<render::ITextureView> rt_view = rt->create_view();

    ViewportLayer viewport;
    viewport.id = "scene";
    viewport.content_rect = shelltest::rect(0, 0, 400, 300);
    viewport.content = rt_view.get();
    viewport.content_size = render::Extent2D{400, 300};
    compositor.publish_viewports({viewport});

    // The full-window CEF layer. Its viewport region is TRANSPARENT (alpha 0) — the "transparent
    // hole" contract that lets native content show through.
    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{1024, 1024},
                                                shelltest::rect(0, 0, 800, 600), 0, 0, 0, 0));

    // The PET_POPUP layer: CEF sends the RECT and the VISIBILITY separately, and the rect commonly
    // arrives before the first popup paint.
    const render::Rect2D popup_rect = shelltest::rect(120, 90, 160, 200);
    compositor.on_popup_state(true, popup_rect);
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{256, 256},
                                         shelltest::rect(0, 0, 160, 200), 255, 255, 255, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);

    CHECK(compositor.render_frame());

    const rendertest::FakePassLog& log = device.pass_log();
    // Three layers drawn, in order: viewport, full-window CEF, popup (03 §4 steps 2-4).
    CHECK(log.draws == 3);
    CHECK(log.scissor_sets == 3);
    CHECK(log.scissors.size() == 3u);
    // 1. the viewport's content rect
    CHECK(shelltest::rect_eq(log.scissors[0], shelltest::rect(0, 0, 400, 300)));
    // 2. the CEF layer spans the WHOLE window. Setting it explicitly is load-bearing: scissor state
    //    persists across draws in a pass, so without it this layer would inherit the viewport's rect.
    CHECK(shelltest::rect_eq(log.scissors[1], shelltest::rect(0, 0, 800, 600)));
    // 3. the popup is CONFINED to its rect — the whole reason set_scissor_rect was added to the RHI.
    CHECK(shelltest::rect_eq(log.scissors[2], popup_rect));

    CHECK(compositor.stats().viewport_draws == 1);
    CHECK(compositor.stats().popup_draws == 1);
    CHECK(compositor.stats().view_frames == 1);
    CHECK(compositor.stats().popup_frames == 1);
}

// The destination SIZE comes from the TEXTURE, never from the scaled DIP size — the second half of
// the conversion, and one a test at 1.5 cannot see: at 1.5 every DIP extent scales to exactly what
// CEF paints, so the two rules agree there and only the origin discriminates. CEF paints
// ceil(DIP x scale) while a scaled DIP size rounds to NEAREST, so 1.25 is where they part.
void test_the_popup_destination_size_comes_from_the_texture_not_the_scaled_dip_size()
{
    WindowCompositor compositor(software_config());
    // 100x60 DIP at 125% == 125x75 physical.
    compositor.attach_cpu(std::make_unique<present::MemoryBlitter>(), render::Extent2D{125, 75});
    compositor.on_resize(render::Extent2D{125, 75}, DpiScale{120});

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{125, 75},
                                                shelltest::rect(0, 0, 125, 75), 0, 0, 0, 255));

    // 21x9 DIP at (8, 4): CEF paints ceil(21 x 1.25) = ceil(26.25) = 27 by ceil(9 x 1.25) = 12,
    // while round(21 x 1.25) = 26 and round(9 x 1.25) = 11 — one short on BOTH axes.
    compositor.on_popup_state(true, shelltest::rect(8, 4, 21, 9));
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{27, 12},
                                         shelltest::rect(0, 0, 27, 12), 255, 128, 64, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);
    CHECK(compositor.render_frame());

    CHECK(shelltest::rect_eq(compositor.popup_dest_rect(), shelltest::rect(10, 5, 27, 12)));

    // THE PIXEL THAT DISTINGUISHES THE TWO RULES: the popup's far corner, at (10+27-1, 5+12-1).
    // A "scale the DIP size" destination is 26x11 and stops one pixel short of it on each axis, so
    // the view shows through the last column and the last row of the menu.
    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    const std::size_t corner = (static_cast<std::size_t>(16) * 125u + 36u) * 4u;
    CHECK(surface[corner + 0] == 255);
    CHECK(surface[corner + 1] == 128);
    CHECK(surface[corner + 2] == 64);
    // ...and one pixel further out is still the view, so the assertion above is a boundary and not
    // a popup that spilled.
    const std::size_t past = (static_cast<std::size_t>(16) * 125u + 37u) * 4u;
    CHECK(surface[past + 0] == 0);
    CHECK(surface[past + 1] == 0);
    CHECK(surface[past + 2] == 0);
}

void test_a_hidden_popup_drops_its_layer()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{400, 300}));

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{512, 512},
                                                shelltest::rect(0, 0, 400, 300), 0, 0, 0, 255));
    compositor.on_popup_state(true, shelltest::rect(10, 10, 100, 100));
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{128, 128},
                                         shelltest::rect(0, 0, 100, 100), 255, 0, 0, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1);
    CHECK(device.pass_log().draws == 2); // view + popup

    // Hiding it DROPS the layer rather than merely stopping the draw: CEF reuses the popup texture
    // for the next dropdown at a different size, so a retained layer would composite the previous
    // menu's pixels for the frame between the hide and the next paint.
    compositor.on_popup_state(false, render::Rect2D{});
    CHECK(!compositor.popup_visible());
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1); // unchanged: no second popup draw
    CHECK(device.pass_log().draws == 3);        // ...and this frame drew the view alone
}

void test_a_layer_clipped_to_nothing_is_skipped_without_disturbing_the_others()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{400, 300}));

    render::TextureDesc rt_desc;
    rt_desc.size = render::Extent2D{100, 100};
    rt_desc.render_attachment = true;
    std::unique_ptr<render::ITexture> rt = device.create_texture(rt_desc);
    std::unique_ptr<render::ITextureView> rt_view = rt->create_view();

    // A viewport panel whose published rect is entirely OFF the window — the state between a layout
    // change and the resize that follows it. It must be skipped, and skipping it must not consume a
    // layer slot: a slot counter advanced past a slot that was never populated is how the NEXT
    // layer ends up binding resources that do not exist.
    ViewportLayer offscreen;
    offscreen.id = "stale";
    offscreen.content_rect = shelltest::rect(900, 900, 100, 100);
    offscreen.content = rt_view.get();
    offscreen.content_size = rt_desc.size;
    compositor.publish_viewports({offscreen});

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_view_frame(storage, render::Extent2D{400, 300},
                                                shelltest::rect(0, 0, 400, 300), 7, 7, 7, 255));
    CHECK(compositor.render_frame());
    // Only the CEF layer drew; the off-window viewport was skipped and NOT counted.
    CHECK(device.pass_log().draws == 1);
    CHECK(compositor.stats().viewport_draws == 0);
    CHECK(shelltest::rect_eq(device.pass_log().scissors[0], shelltest::rect(0, 0, 400, 300)));
}

// The PARTIALLY clipped layer — the case between "fully visible" and "fully off-window", and the one
// that was wrong.
//
// Regression: draw_layer derived the UV from the CLIPPED destination rect instead of the full one,
// which maps the whole source across the narrowed span — so a layer straddling a window edge was
// SQUEEZED to fit rather than cropped. The scissor is what crops; the UV must be extrapolated from
// the layer's true rect or the two disagree. Nothing caught it because the sibling test only covers
// a rect that clips to NOTHING (skipped entirely, no UV computed).
void test_a_partially_clipped_layer_is_cropped_not_squeezed()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    const render::Extent2D window{400, 300};
    CHECK(compositor.attach_gpu(device, surface, window));

    render::TextureDesc rt_desc;
    rt_desc.size = render::Extent2D{200, 100};
    rt_desc.render_attachment = true;
    std::unique_ptr<render::ITexture> rt = device.create_texture(rt_desc);
    std::unique_ptr<render::ITextureView> rt_view = rt->create_view();

    // 200 wide at x=300 in a 400-wide window: the right HALF hangs off the edge.
    ViewportLayer straddling;
    straddling.id = "straddling";
    straddling.content_rect = shelltest::rect(300, 0, 200, 100);
    straddling.content = rt_view.get();
    straddling.content_size = rt_desc.size;
    compositor.publish_viewports({straddling});

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(
        make_view_frame(storage, window, shelltest::rect(0, 0, 400, 300), 7, 7, 7, 255));
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().viewport_draws == 1);

    // The SCISSOR is clipped to what is on-window — that is what crops the layer.
    CHECK(shelltest::rect_eq(device.pass_log().scissors[0], shelltest::rect(300, 0, 100, 100)));

    // ...and the UV is extrapolated from the layer's FULL 200-wide rect, so the visible left half
    // samples the source's left half at its native scale. The uniform payload the compositor
    // uploaded is the only place this is observable.
    CHECK(!device.pass_log().buffer_writes.empty());
    present::CompositeUv uv{};
    const std::vector<std::uint8_t>& payload = device.pass_log().buffer_writes.front();
    CHECK(payload.size() == sizeof(uv));
    if (payload.size() == sizeof(uv))
    {
        std::memcpy(&uv, payload.data(), sizeof(uv));
    }
    // u spans the full source across 200 destination pixels, i.e. 1/200 per pixel. Extrapolated back
    // to screen x=0 that is u0 = -300/200 = -1.5, and forward to x=400, u1 = u0 + 400/200 = 0.5.
    // The BUG produced du = 1/100 (the clipped width), giving u0 = -3.0 and u1 = 1.0 — the source
    // compressed into the surviving 100 pixels.
    CHECK(shelltest::near_eq(uv.u0, -1.5f));
    CHECK(shelltest::near_eq(uv.u1, 0.5f));
}

void test_resize_protocol()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    rendertest::FakeSwapchain* swapchain = surface.last_swapchain();
    CHECK(swapchain != nullptr);

    const DpiScale one_x{kReferenceDpi};
    compositor.on_resize(render::Extent2D{1024, 768}, one_x);
    CHECK(shelltest::extent_eq(compositor.size(), render::Extent2D{1024, 768}));
    CHECK(swapchain->resize_count() == 1);
    CHECK(compositor.damage().resize);

    // A minimized window reports 0x0 EVERY FRAME. Tearing the chain down and rebuilding it each
    // time would thrash; the zero extent is ignored and the last valid configuration kept.
    compositor.on_resize(render::Extent2D{0, 0}, one_x);
    CHECK(shelltest::extent_eq(compositor.size(), render::Extent2D{1024, 768}));
    CHECK(swapchain->resize_count() == 1);

    // An UNCHANGED size is a no-op: a spurious WM_SIZE must not force a reconfigure.
    compositor.on_resize(render::Extent2D{1024, 768}, one_x);
    CHECK(swapchain->resize_count() == 1);

    // a2: the SCALE rides the same call, and it is taken even when the SIZE is unchanged — a user
    // changing the desktop scaling of a maximized window is exactly that shape. It must NOT
    // reconfigure the swapchain (nothing about the backbuffer changed) and it MUST damage the frame,
    // because the popup destination is derived from the scale per frame.
    const DpiScale one_five{144};
    compositor.on_resize(render::Extent2D{1024, 768}, one_five);
    CHECK(compositor.dpi() == one_five);
    CHECK(swapchain->resize_count() == 1);
    CHECK(compositor.damage().popup);

    // ...and it is taken even while MINIMIZED, AHEAD of the zero-extent guard: a scale dropped there
    // would composite the next dropdown at the old factor once the window came back.
    compositor.on_resize(render::Extent2D{0, 0}, DpiScale{192});
    CHECK(compositor.dpi() == (DpiScale{192}));
    CHECK(shelltest::extent_eq(compositor.size(), render::Extent2D{1024, 768}));
    CHECK(swapchain->resize_count() == 1);
}

// ------------------------------------------------------------- a2: the popup rect is DIP, not px
//
// THE SET'S NAMED VACUITY GATE. `OnPopupSize` reports the popup in DIP (view coordinates) while its
// OnPaint texture is PHYSICAL, and the compositor used the DIP rect as the physical destination. At
// scale 1.0 the correct and the broken code are BYTE-IDENTICAL — which is exactly why this shipped
// green through every CI leg — so the assertions that matter are the ones at 1.5. The 1.0 cases in
// test_layer_order_and_the_popup_scissor and test_cpu_path_composites_the_popup_rather_than_
// skipping_it are deliberately kept beside these: they pin the identity path.

void test_gpu_path_composites_the_popup_at_its_physical_rect_at_scale_1_5()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    // An 800x600 DIP view on a 150% monitor: 1200x900 physical.
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{1200, 900}));
    const DpiScale one_five{144};
    compositor.on_resize(render::Extent2D{1200, 900}, one_five);

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{2048, 2048},
                                                shelltest::rect(0, 0, 1200, 900), 0, 0, 0, 255));

    // A dropdown CEF places at 260x400 DIP, 200x120 DIP big — and paints as a 300x180 PHYSICAL
    // texture. The two halves of the split, in one frame.
    const render::Rect2D popup_dip = shelltest::rect(260, 400, 200, 120);
    compositor.on_popup_state(true, popup_dip);
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{512, 512},
                                         shelltest::rect(0, 0, 300, 180), 255, 255, 255, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);

    CHECK(compositor.render_frame());

    const rendertest::FakePassLog& log = device.pass_log();
    CHECK(log.draws == 2); // the view, then the popup
    CHECK(log.scissors.size() == 2u);

    // THE ASSERTION: the popup is scissored to its PHYSICAL rect — the DIP origin scaled by 1.5,
    // sized by the TEXTURE. (390, 600) 300x180, not the (260, 400) 200x120 CEF reported.
    const render::Rect2D want = shelltest::rect(390, 600, 300, 180);
    CHECK(shelltest::rect_eq(log.scissors[1], want));
    CHECK(shelltest::rect_eq(compositor.popup_dest_rect(), want));

    // ...and the shipped bug is a DIFFERENT rect on BOTH axes and BOTH extents, so none of the four
    // numbers above can be right by accident.
    CHECK(!shelltest::rect_eq(log.scissors[1], popup_dip));
    CHECK(log.scissors[1].origin.x != popup_dip.origin.x);
    CHECK(log.scissors[1].origin.y != popup_dip.origin.y);
    CHECK(log.scissors[1].size.width != popup_dip.size.width);
    CHECK(log.scissors[1].size.height != popup_dip.size.height);
    // The RAW rect is still reported as CEF sent it — the conversion is at the DESTINATION, not at
    // delivery, so a later DPI change re-derives instead of compounding.
    CHECK(shelltest::rect_eq(compositor.popup_rect(), popup_dip));

    // ONE TEXEL PER PIXEL over the popup too: 300 physical columns span 300/512 of the allocation.
    // The bug squeezed the same 300-column texture into a 200-pixel destination, which resamples
    // every glyph in the menu rather than cropping it.
    CHECK(log.buffer_writes.size() == 2u);
    present::CompositeUv uv;
    std::memcpy(&uv, log.buffer_writes[1].data(), sizeof(uv));
    const float du = (uv.u1 - uv.u0) / static_cast<float>(compositor.size().width);
    CHECK(shelltest::near_eq(du * 300.0f, 300.0f / 512.0f));

    // A DPI change with the popup ALREADY OPEN moves it, with no new callback from the browser —
    // the reason the DIP rect is stored raw and converted per frame.
    compositor.on_resize(render::Extent2D{1600, 1200}, DpiScale{192});
    CHECK(compositor.render_frame());
    CHECK(log.scissors.size() == 4u);        // view + popup again
    CHECK(log.scissors[3].origin.x == 520u); // 260 x 2.0
    CHECK(log.scissors[3].origin.y == 800u); // 400 x 2.0
}

void test_cpu_path_composites_the_popup_at_its_physical_rect_at_scale_1_5()
{
    WindowCompositor compositor(software_config());
    // 80x60 DIP at 150% == 120x90 physical. Small enough to address per pixel below.
    compositor.attach_cpu(std::make_unique<present::MemoryBlitter>(), render::Extent2D{120, 90});
    compositor.on_resize(render::Extent2D{120, 90}, DpiScale{144});

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{120, 90},
                                                shelltest::rect(0, 0, 120, 90), 0, 0, 0, 255));

    // 20x10 DIP at (10, 5) DIP -> 30x15 physical at (15, 8) physical (5 x 1.5 = 7.5, rounded away
    // from zero to 8 — the same round-to-nearest every other conversion in dpi.h uses).
    const render::Rect2D popup_dip = shelltest::rect(10, 5, 20, 10);
    compositor.on_popup_state(true, popup_dip);
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{30, 15},
                                         shelltest::rect(0, 0, 30, 15), 255, 128, 64, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);

    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1);
    CHECK(shelltest::rect_eq(compositor.popup_dest_rect(), shelltest::rect(15, 8, 30, 15)));

    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    const auto at = [](std::uint32_t x, std::uint32_t y) {
        return (static_cast<std::size_t>(y) * 120u + x) * 4u;
    };
    const auto is_popup = [&surface, &at](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = at(x, y);
        return surface[i + 0] == 255 && surface[i + 1] == 128 && surface[i + 2] == 64;
    };
    const auto is_view = [&surface, &at](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = at(x, y);
        return surface[i + 0] == 0 && surface[i + 1] == 0 && surface[i + 2] == 0 &&
               surface[i + 3] == 255;
    };

    // INSIDE the physical rect, on all four corners — the popup's pixels.
    CHECK(is_popup(15, 8));  // top-left
    CHECK(is_popup(44, 8));  // top-right    (15 + 30 - 1)
    CHECK(is_popup(15, 22)); // bottom-left  (8 + 15 - 1)
    CHECK(is_popup(44, 22)); // bottom-right
    // OUTSIDE it, one pixel past each edge — the view's black. Asserting BOTH is what distinguishes
    // a real second layer from a popup that was dropped (view everywhere) or drawn full-window
    // (popup everywhere), the discipline docs/shell.md already records for this test.
    CHECK(is_view(14, 8));
    CHECK(is_view(45, 8));
    CHECK(is_view(15, 7));
    CHECK(is_view(15, 23));
    CHECK(is_view(0, 0));

    // THE SHIPPED BUG, sampled directly: it blitted the DIP rect (10, 5) 20x10, which put popup
    // pixels at (10, 5) and left the true bottom-right quadrant showing the view. Both of these
    // lines flip if the conversion is removed, and NEITHER can flip at scale 1.0 — there the DIP
    // rect and the physical rect are the same four numbers.
    CHECK(is_view(10, 5));   // the OLD origin is outside the popup now
    CHECK(is_popup(40, 20)); // ...and the far corner the old 20x10 blit never reached
}

void test_outdated_and_lost_reconfigure_and_keep_the_damage()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    rendertest::FakeSwapchain* swapchain = surface.last_swapchain();

    // Outdated/Lost are NORMAL (a resize raced the frame; the device went away), not errors.
    swapchain->script_acquire(render::AcquireStatus::Outdated);
    CHECK(!compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 0);
    CHECK(compositor.stats().reconfigures == 1);
    // THE LOAD-BEARING PART: the damage SURVIVES a failed frame. Clearing it would blank the editor
    // until something else happened to damage it — which, in a damage-driven shell that has just
    // gone idle, can be never.
    CHECK(compositor.damage().any());

    swapchain->script_acquire(render::AcquireStatus::Lost);
    CHECK(!compositor.render_frame());
    CHECK(compositor.stats().reconfigures == 2);
    CHECK(compositor.damage().any());

    // Recovery: the next acquire succeeds and the retained damage is finally presented.
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 1);
    CHECK(!compositor.damage().any());

    // Timeout/Error are reported but do NOT reconfigure.
    compositor.mark_external_damage();
    swapchain->script_acquire(render::AcquireStatus::Timeout);
    CHECK(!compositor.render_frame());
    CHECK(compositor.stats().reconfigures == 2);
    CHECK(compositor.stats().acquire_failures == 3);
}

void test_suboptimal_presents_first_then_reconfigures()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    rendertest::FakeSwapchain* swapchain = surface.last_swapchain();
    swapchain->set_suboptimal(true);

    // Suboptimal is the one status where the frame IS presentable and must be presented, THEN
    // reconfigured. Treating it like Outdated would drop a good frame on every pending resize.
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().frames_presented == 1);
    CHECK(swapchain->present_count() == 1);
    CHECK(compositor.stats().reconfigures == 1);
}

void test_attach_gpu_refuses_an_unpresentable_surface()
{
    rendertest::FakeDevice device;
    render::SurfaceCaps caps; // supported == false
    rendertest::FakeSurface surface(caps);
    WindowCompositor compositor(software_config());
    CHECK(!compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    CHECK(compositor.path() == PresentPath::none);
    CHECK(shelltest::mentions(compositor.diagnostic(), "CPU present path"));

    // A surface that IS supported but lacks the format is refused too — e03's contract is that
    // configure() REFUSES rather than substituting, so an unchecked configure is a null swapchain.
    render::SurfaceCaps partial;
    partial.supported = true;
    partial.formats = {render::TextureFormat::RGBA8Unorm};
    partial.present_modes = {render::PresentMode::Fifo};
    rendertest::FakeSurface no_bgra(partial);
    WindowCompositor second(software_config());
    CHECK(!second.attach_gpu(device, no_bgra, render::Extent2D{800, 600}));
    CHECK(shelltest::mentions(second.diagnostic(), "BGRA8Unorm"));
}

// -------------------------------------------------------------- the CPU present fallback (C-F2)

void test_cpu_path_composes_and_presents_through_the_blitter()
{
    WindowCompositor compositor(software_config());
    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* raw = blitter.get();
    compositor.attach_cpu(std::move(blitter), render::Extent2D{200, 100});
    CHECK(compositor.path() == PresentPath::cpu_blit);
    CHECK(compositor.diagnostic().empty());

    // With no frame yet there is nothing to present.
    CHECK(!compositor.render_frame());

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_view_frame(storage, render::Extent2D{100, 50},
                                                shelltest::rect(0, 0, 100, 50), 10, 20, 30, 255));
    CHECK(compositor.render_frame());
    CHECK(raw->blit_count() == 1);
    CHECK(shelltest::extent_eq(raw->target_size(), render::Extent2D{200, 100}));

    // The composed surface is WINDOW-sized (the 1:1 rule) and carries the browser's pixels from
    // the origin — this is the "software-OSR path works" assertion, at the level of actual bytes.
    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    CHECK(surface.size() == static_cast<std::size_t>(200) * 100 * 4);
    CHECK(surface[0] == 10);
    CHECK(surface[1] == 20);
    CHECK(surface[2] == 30);
    CHECK(surface[3] == 255);
    // ...and the blit was 1:1, never an aspect fit of a smaller image.
    CHECK(!raw->last_plan().letterboxed);
}

// A coordinate-encoding view frame (B = x, G = y) so a crop, an offset or a scale is identifiable.
BrowserFrame make_coordinate_view_frame(std::vector<std::uint8_t>& storage, render::Extent2D size)
{
    storage.assign(static_cast<std::size_t>(size.width) * size.height * 4u, 0u);
    for (std::uint32_t y = 0; y < size.height; ++y)
    {
        for (std::uint32_t x = 0; x < size.width; ++x)
        {
            std::uint8_t* p = storage.data() + (static_cast<std::size_t>(y) * size.width + x) * 4u;
            p[0] = static_cast<std::uint8_t>(x);
            p[1] = static_cast<std::uint8_t>(y);
            p[2] = 0x7F;
            p[3] = 0xFF;
        }
    }
    BrowserFrame frame;
    frame.layer = BrowserLayer::view;
    frame.frame.pixels = storage.data();
    frame.frame.byte_size = storage.size();
    frame.frame.bytes_per_row = size.width * 4u;
    frame.frame.coded_size = size;
    frame.frame.visible_rect = render::Rect2D{render::Origin2D{}, size};
    return frame;
}

[[nodiscard]] const std::uint8_t* surface_px(const std::vector<std::uint8_t>& surface,
                                             render::Extent2D size, std::uint32_t x,
                                             std::uint32_t y)
{
    return surface.data() + (static_cast<std::size_t>(y) * size.width + x) * 4u;
}

void test_cpu_path_presents_the_view_1_to_1_cropped_to_the_window()
{
    // THE DPI-ROUNDING SHAPE: the browser painted one pixel more than the client on both axes
    // (ceil(DIP × 1.5) against a round-to-nearest DIP rect). The frame is CROPPED, never fitted:
    // every window pixel is the frame's pixel at the same coordinate, the extra column and row are
    // dropped, and the blitter sees an image of exactly the window's size — the plant that reddens
    // this is the old aspect-fit compose (surface 5x4, plan letterboxed, a black bar on screen).
    WindowCompositor compositor(software_config());
    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* raw = blitter.get();
    const render::Extent2D window{4, 3};
    compositor.attach_cpu(std::move(blitter), window);

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_coordinate_view_frame(storage, render::Extent2D{5, 4}));
    CHECK(compositor.render_frame());

    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    CHECK(surface.size() == static_cast<std::size_t>(4) * 3 * 4);
    CHECK(surface_px(surface, window, 0, 0)[0] == 0 && surface_px(surface, window, 0, 0)[1] == 0);
    CHECK(surface_px(surface, window, 3, 2)[0] == 3 && surface_px(surface, window, 3, 2)[1] == 2);
    CHECK(surface_px(surface, window, 3, 2)[3] == 0xFF);
    CHECK(shelltest::extent_eq(raw->target_size(), window));
    CHECK(!raw->last_plan().letterboxed);
    CHECK(raw->last_plan().width == 4 && raw->last_plan().height == 3);
}

void test_cpu_path_fills_an_uncovered_remainder_with_the_clear_colour()
{
    // The transient after a resize: the window grew, the browser has not repainted yet, so its
    // frame is SMALLER than the window. The frame still lands 1:1 at the origin (no stretch, no
    // centring) and the strip it does not reach is the clear colour — opaque black, so nothing of
    // the desktop and nothing of a previous frame shows through.
    WindowCompositor compositor(software_config());
    auto blitter = std::make_unique<present::MemoryBlitter>();
    present::MemoryBlitter* raw = blitter.get();
    const render::Extent2D window{4, 3};
    compositor.attach_cpu(std::move(blitter), window);

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_coordinate_view_frame(storage, render::Extent2D{3, 2}));
    CHECK(compositor.render_frame());

    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    CHECK(surface.size() == static_cast<std::size_t>(4) * 3 * 4);
    // Covered: the frame's own pixel at the same coordinate.
    CHECK(surface_px(surface, window, 2, 1)[0] == 2 && surface_px(surface, window, 2, 1)[1] == 1);
    // Uncovered column and row: the clear colour, premultiplied BGRA (0, 0, 0, 255).
    const std::uint8_t* right = surface_px(surface, window, 3, 0);
    CHECK(right[0] == 0 && right[1] == 0 && right[2] == 0 && right[3] == 0xFF);
    const std::uint8_t* below = surface_px(surface, window, 0, 2);
    CHECK(below[0] == 0 && below[1] == 0 && below[2] == 0 && below[3] == 0xFF);
    // And the blit is still the identity — the CLEAR is what fills the window, never a fit.
    CHECK(!raw->last_plan().letterboxed);
}

void test_gpu_path_draws_the_view_1_to_1_cropped_to_the_window()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));

    // The same DPI-rounding shape on the GPU path: one pixel more than the client on both axes.
    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_view_frame(storage, render::Extent2D{1024, 1024},
                                                shelltest::rect(0, 0, 801, 601), 0, 0, 0, 255));
    CHECK(compositor.render_frame());

    const rendertest::FakePassLog& log = device.pass_log();
    CHECK(log.draws == 1);
    CHECK(log.scissors.size() == 1u);
    CHECK(shelltest::rect_eq(log.scissors[0], shelltest::rect(0, 0, 800, 600)));
    // ONE TEXEL PER PIXEL: the window's 800 columns span 800/1024 of the allocation — not the
    // 801/1024 a whole-visible-rect stretch uploads, which is the plant that reddens this line.
    CHECK(log.buffer_writes.size() == 1u);
    CHECK(log.buffer_writes[0].size() == sizeof(present::CompositeUv));
    present::CompositeUv uv;
    std::memcpy(&uv, log.buffer_writes[0].data(), sizeof(uv));
    CHECK(shelltest::near_eq(uv.u0, 0.0f));
    CHECK(shelltest::near_eq(uv.v0, 0.0f));
    CHECK(shelltest::near_eq(uv.u1, 800.0f / 1024.0f));
    CHECK(shelltest::near_eq(uv.v1, 600.0f / 1024.0f));

    // A frame SMALLER than the window (the post-resize transient) is drawn 1:1 too: the SCISSOR
    // shrinks to the frame and the rest keeps the pass clear, rather than the frame being stretched.
    // The uploaded UV is still the window's one-texel-per-pixel mapping (compute_layer_uv
    // extrapolates it across the whole target so the interpolation is right INSIDE the scissor) —
    // a stretch would upload 799/1024 here.
    std::vector<std::uint8_t> smaller;
    compositor.on_browser_frame(make_view_frame(smaller, render::Extent2D{1024, 1024},
                                                shelltest::rect(0, 0, 799, 599), 0, 0, 0, 255));
    CHECK(compositor.render_frame());
    CHECK(log.scissors.size() == 2u);
    CHECK(shelltest::rect_eq(log.scissors[1], shelltest::rect(0, 0, 799, 599)));
    CHECK(log.buffer_writes.size() == 2u);
    std::memcpy(&uv, log.buffer_writes[1].data(), sizeof(uv));
    CHECK(shelltest::near_eq(uv.u1, 800.0f / 1024.0f));
    CHECK(shelltest::near_eq(uv.v1, 600.0f / 1024.0f));
}

void test_cpu_path_composites_the_popup_rather_than_skipping_it()
{
    WindowCompositor compositor(software_config());
    compositor.attach_cpu(std::make_unique<present::MemoryBlitter>(), render::Extent2D{100, 50});

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{100, 50},
                                                shelltest::rect(0, 0, 100, 50), 0, 0, 0, 255));
    const render::Rect2D popup_rect = shelltest::rect(10, 5, 20, 10);
    compositor.on_popup_state(true, popup_rect);
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{20, 10},
                                         shelltest::rect(0, 0, 20, 10), 255, 128, 64, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);

    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1);

    // Inside the popup rect: the popup's opaque pixels.
    const std::vector<std::uint8_t>& surface = compositor.cpu_surface();
    const std::size_t inside = (static_cast<std::size_t>(5) * 100 + 10) * 4;
    CHECK(surface[inside + 0] == 255);
    CHECK(surface[inside + 1] == 128);
    CHECK(surface[inside + 2] == 64);
    // Outside it: the view's black. A GPU-less fallback that silently dropped the popup would show
    // black here too, so the sample INSIDE is what distinguishes the two.
    CHECK(surface[0] == 0);
    CHECK(surface[3] == 255);
}

void test_cpu_path_without_a_blitter_is_reported_not_silent()
{
    WindowCompositor compositor(software_config());
    // A build with no 2D primitive for this platform: the shell still runs, and says WHY.
    compositor.attach_cpu(nullptr, render::Extent2D{100, 50});
    CHECK(compositor.path() == PresentPath::cpu_blit);
    CHECK(!compositor.diagnostic().empty());
    // The diagnostic must name the ACTIONABLE cause for each platform that can still reach it: on
    // Linux the missing X11 development headers (a package the reader can install), and on macOS a
    // window carrying no CAMetalLayer. Since e12b, NEITHER platform is owed a blitter by a future
    // task.
    //
    // ⚠ THE ROT HAPPENED TWICE, so the fix is now to assert NO task id at all. Asserting a bare
    // "e12" — as this test did until e12a — passed silently because "e12" is a prefix of "e12b",
    // while the message went on telling Linux users to wait for a task that had shipped. Pinning
    // "e12b" instead fixed that generation and then rotted the SAME WAY the moment e12b itself
    // shipped: still green, still advising a wait for finished work. Any task id in this string is
    // therefore the defect, so pin the two substrings that carry meaning AND assert the absence of
    // the whole "e12" family — that last check is the one that cannot rot with the next milestone.
    CHECK(shelltest::mentions(compositor.diagnostic(), "X11"));
    CHECK(shelltest::mentions(compositor.diagnostic(), "macOS"));
    CHECK(!shelltest::mentions(compositor.diagnostic(), "e12"));

    std::vector<std::uint8_t> storage;
    compositor.on_browser_frame(make_view_frame(storage, render::Extent2D{100, 50},
                                                shelltest::rect(0, 0, 100, 50), 1, 2, 3, 255));
    CHECK(!compositor.render_frame()); // nothing presented, nothing crashed
}

void test_a_malformed_producer_frame_is_refused()
{
    WindowCompositor compositor(software_config());
    compositor.attach_cpu(std::make_unique<present::MemoryBlitter>(), render::Extent2D{100, 50});

    // A frame claiming more rows than its buffer holds. A blitter reads height*bytes_per_row bytes
    // on trust, so without the shared bounds rule this is an out-of-bounds read, not a refusal.
    std::vector<std::uint8_t> truncated(16, 0u);
    BrowserFrame frame;
    frame.frame.pixels = truncated.data();
    frame.frame.byte_size = truncated.size();
    frame.frame.bytes_per_row = 400;
    frame.frame.coded_size = render::Extent2D{100, 50};
    frame.frame.visible_rect = shelltest::rect(0, 0, 100, 50);
    compositor.on_browser_frame(frame);
    CHECK(!compositor.render_frame());
}

// A REFUSED popup frame must not move the geometry `popup_dest_rect()` is built from. The rect is
// half DIP input and half TEXTURE extent (a2), so recording a refused frame's extent would have the
// accessor — documented in compositor.h as describing the texture each path is holding — report a
// menu whose pixels were never stored, and would decouple the CPU path's destination SIZE (read
// from that pair) from its source pointer and stride (read from `cpu_popup_`). Only
// `capture_cpu_frame`'s leading `valid = false` kept that from being an out-of-bounds source bound.
void test_a_refused_popup_frame_leaves_the_last_good_geometry_in_place()
{
    WindowCompositor compositor(software_config());
    compositor.attach_cpu(std::make_unique<present::MemoryBlitter>(), render::Extent2D{120, 90});
    compositor.on_resize(render::Extent2D{120, 90}, DpiScale{144});

    std::vector<std::uint8_t> view_storage;
    compositor.on_browser_frame(make_view_frame(view_storage, render::Extent2D{120, 90},
                                                shelltest::rect(0, 0, 120, 90), 0, 0, 0, 255));

    // A GOOD popup first: 20x10 DIP at (10, 5) painted as a 30x15 physical texture -> (15, 8) 30x15.
    compositor.on_popup_state(true, shelltest::rect(10, 5, 20, 10));
    std::vector<std::uint8_t> popup_storage;
    BrowserFrame popup = make_view_frame(popup_storage, render::Extent2D{30, 15},
                                         shelltest::rect(0, 0, 30, 15), 255, 128, 64, 255);
    popup.layer = BrowserLayer::popup;
    compositor.on_browser_frame(popup);
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1);
    const render::Rect2D good = shelltest::rect(15, 8, 30, 15);
    CHECK(shelltest::rect_eq(compositor.popup_dest_rect(), good));

    // ...then one claiming a BIGGER allocation than its buffer holds. Deliberately a different
    // extent from the good frame on BOTH axes: recording it would make the assertion below read
    // 60x30, so this cannot pass by accident.
    std::vector<std::uint8_t> truncated(16, 0u);
    BrowserFrame refused;
    refused.layer = BrowserLayer::popup;
    refused.frame.pixels = truncated.data();
    refused.frame.byte_size = truncated.size();
    refused.frame.bytes_per_row = 240;
    refused.frame.coded_size = render::Extent2D{60, 30};
    refused.frame.visible_rect = shelltest::rect(0, 0, 60, 30);
    compositor.on_browser_frame(refused);

    // It REACHED the popup branch — otherwise the assertion below would hold for the wrong reason
    // (a frame refused one layer earlier never had a chance to overwrite anything).
    CHECK(compositor.stats().popup_frames == 2);
    CHECK(shelltest::rect_eq(compositor.popup_dest_rect(), good));
    // ...and no popup is composited from a frame whose pixels were never stored.
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().popup_draws == 1);
}

void test_detach_is_safe_and_idempotent()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));
    CHECK(surface.last_swapchain() != nullptr);
    compositor.detach();
    CHECK(compositor.path() == PresentPath::none);
    // Read the count off the SURFACE, never off a swapchain pointer captured before detach():
    // detach() unconfigures AND destroys the swapchain (compositor.cpp — swapchain_.reset()), so
    // such a pointer dangles here. The surface outlives every swapchain it hands out, which is what
    // keeps "detach() really unconfigured, it did not merely drop the pointer" observable.
    CHECK(surface.unconfigure_count() == 1);
    compositor.detach(); // idempotent
    // Idempotent means the SECOND detach unconfigured nothing — there was no swapchain left to
    // unconfigure — rather than running the teardown twice.
    CHECK(surface.unconfigure_count() == 1);
    CHECK(!compositor.render_frame());
}

} // namespace

int main()
{
    test_layer_uv_full_window_is_the_identity();
    test_layer_uv_extrapolates_so_a_sub_rect_samples_correctly();
    test_layer_uv_honours_a_visible_sub_rect();
    test_layer_uv_degenerate_inputs_fall_back_rather_than_divide_by_zero();
    test_premultiplied_blend();
    test_premultiplied_blend_clips_at_the_destination_edges();
    test_damage_driven_redraw_skips_an_undamaged_frame();
    test_layer_order_and_the_popup_scissor();
    test_gpu_path_composites_the_popup_at_its_physical_rect_at_scale_1_5();
    test_cpu_path_composites_the_popup_at_its_physical_rect_at_scale_1_5();
    test_the_popup_destination_size_comes_from_the_texture_not_the_scaled_dip_size();
    test_a_hidden_popup_drops_its_layer();
    test_a_layer_clipped_to_nothing_is_skipped_without_disturbing_the_others();
    test_a_partially_clipped_layer_is_cropped_not_squeezed();
    test_resize_protocol();
    test_outdated_and_lost_reconfigure_and_keep_the_damage();
    test_suboptimal_presents_first_then_reconfigures();
    test_attach_gpu_refuses_an_unpresentable_surface();
    test_cpu_path_composes_and_presents_through_the_blitter();
    test_cpu_path_presents_the_view_1_to_1_cropped_to_the_window();
    test_cpu_path_fills_an_uncovered_remainder_with_the_clear_colour();
    test_gpu_path_draws_the_view_1_to_1_cropped_to_the_window();
    test_cpu_path_composites_the_popup_rather_than_skipping_it();
    test_cpu_path_without_a_blitter_is_reported_not_silent();
    test_a_malformed_producer_frame_is_refused();
    test_a_refused_popup_frame_leaves_the_last_good_geometry_in_place();
    test_detach_is_safe_and_idempotent();
    SHELL_TEST_MAIN_END();
}
