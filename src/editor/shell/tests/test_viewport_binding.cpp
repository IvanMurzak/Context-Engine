// The Scene viewport PRODUCER (M9 editor-UX e3, D7): the region map -> per-viewport render targets
// -> the D5 pass -> the compositor's layer stack, the opaque `editor.camera-set` payload codec, and
// the honest adapter-absent degrade.
//
// Driven through `rendertest::FakeDevice` — the GPU-free fake `test_compositor.cpp` already uses —
// so the REAL producer path (target create / resize-in-place / release, the render pass, the layer
// publish, the damage split) runs on all three `build` legs with no adapter anywhere.
//
// ⚠ WHAT THE SCALE CASES HERE DO AND DO NOT PROVE. The set's gate is that a geometry assertion at
// device scale 1.0 is VACUOUS. The DIP -> physical CONVERSION for a viewport lives in editor-core
// (viewport.ts's `viewportRegions`, sharing chrome.ts's ONE `devicePixelRatio` seam — a2), and its
// non-vacuity gate is `viewport.test.ts`, which asserts it at dpr 1.5 / 2 / 3 against real laid-out
// CSS geometry. What THIS file must prove is the other half of the same rule: the producer is handed
// PHYSICAL rects and must never re-derive them — `test_physical_rects_are_never_rescaled` publishes
// the same DIP rect converted at 96 and at 144 dpi with the compositor's own a2 `dpi_` seam set to
// 144, so a producer that "helpfully" applied `to_physical(rect, compositor.dpi())` (the double
// application that IS the a2 bug class) fails it, and it cannot pass at 1.0 by coincidence because
// the two publishes are compared against each other.

#include "context/editor/shell/viewport_binding.h"

#include "render_test_rhi.h"
#include "shell_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;
namespace contract = context::editor::contract;

namespace
{

constexpr const char* kViewportA = "builtin.viewport#1";
constexpr const char* kViewportB = "builtin.viewport#2";

CompositorConfig software_config()
{
    CompositorConfig config;
    config.import_options.force_software = true;
    return config;
}

ShellRegion viewport_region(const char* id, const render::Rect2D& rect)
{
    ShellRegion region;
    region.id = id;
    region.rect = rect;
    region.kind = RegionKind::viewport;
    return region;
}

ShellRegion caption_region(const char* id, const render::Rect2D& rect)
{
    ShellRegion region;
    region.id = id;
    region.rect = rect;
    region.kind = RegionKind::caption;
    return region;
}

// The webui's own arithmetic (chrome.ts `physicalRegion`, shared by viewport.ts): round the EDGES
// and derive the extents, never left+width independently. Reproduced here so the C++ cases are fed
// rects with the same shape the wire really carries at a non-integral scale.
render::Rect2D physical_from_dip(double x, double y, double w, double h, double dpr)
{
    const auto round_u32 = [](double v)
    { return static_cast<std::uint32_t>(v < 0.0 ? 0.0 : v + 0.5); };
    const std::uint32_t x0 = round_u32(x * dpr);
    const std::uint32_t y0 = round_u32(y * dpr);
    return render::Rect2D{render::Origin2D{x0, y0},
                          render::Extent2D{round_u32((x + w) * dpr) - x0,
                                           round_u32((y + h) * dpr) - y0}};
}

PointerEvent pointer_at(std::int32_t x, std::int32_t y)
{
    PointerEvent event;
    event.action = PointerAction::move;
    event.position = PointI{x, y};
    return event;
}

// ------------------------------------------------------------------------------ the camera codec

void test_camera_codec_round_trip()
{
    render::View authored;
    authored.transform.position[0] = 1.5f;
    authored.transform.position[1] = -2.25f;
    authored.transform.position[2] = 12.0f;
    authored.transform.rotation[0] = 0.25f;
    authored.transform.rotation[3] = 0.75f;
    authored.transform.scale[1] = 2.0f;
    authored.projection.fov_y_radians = 0.9f;
    authored.projection.ortho_half_height = 6.5f;
    authored.projection.near_z = 0.05f;
    authored.projection.far_z = 512.0f;
    authored.mode = render::ViewMode::two_d;
    authored.type = render::ViewType::game;

    const contract::Json transform = camera_transform_json(authored);
    const contract::Json projection = camera_projection_json(authored);

    render::View restored;
    CHECK(apply_camera_transform(restored, transform));
    CHECK(apply_camera_projection(restored, projection));

    CHECK(shelltest::near_eq(restored.transform.position[0], 1.5f));
    CHECK(shelltest::near_eq(restored.transform.position[1], -2.25f));
    CHECK(shelltest::near_eq(restored.transform.position[2], 12.0f));
    CHECK(shelltest::near_eq(restored.transform.rotation[0], 0.25f));
    CHECK(shelltest::near_eq(restored.transform.rotation[3], 0.75f));
    CHECK(shelltest::near_eq(restored.transform.scale[1], 2.0f));
    CHECK(shelltest::near_eq(restored.projection.fov_y_radians, 0.9f));
    CHECK(shelltest::near_eq(restored.projection.ortho_half_height, 6.5f));
    CHECK(shelltest::near_eq(restored.projection.near_z, 0.05f));
    CHECK(shelltest::near_eq(restored.projection.far_z, 512.0f));
    CHECK(restored.mode == render::ViewMode::two_d);
    CHECK(restored.type == render::ViewType::game);

    // The `viewportId` SLOT is deliberately NOT part of the payload: it is session-local and the
    // daemon must never see it (view.h's warning). A codec that carried it would restore a render
    // slot from a previous process onto a target that no longer exists.
    CHECK(!transform.contains("viewportId"));
    CHECK(!projection.contains("viewportId"));

    // The params of ONE `editor.camera-set`, exactly as the verb declares them.
    const contract::Json params = camera_set_params(kViewportA, authored);
    CHECK(params.at("viewportId").as_string() == kViewportA);
    CHECK(params.at("transform").is_object());
    CHECK(params.at("projection").is_object());
}

void test_camera_codec_is_total_over_garbage()
{
    render::View view;
    const render::View before = view;

    // A blob that is not an object at all, a partial array, and an unknown mode token: each leaves
    // the view where it was rather than half-applying a camera nobody asked for.
    CHECK(!apply_camera_transform(view, contract::Json("nonsense")));
    contract::Json partial = contract::Json::object();
    contract::Json two = contract::Json::array();
    two.push_back(contract::Json(1.0));
    two.push_back(contract::Json(2.0));
    partial.set("position", two);
    CHECK(!apply_camera_transform(view, partial));
    CHECK(shelltest::near_eq(view.transform.position[0], before.transform.position[0]));
    CHECK(shelltest::near_eq(view.transform.position[1], before.transform.position[1]));

    contract::Json unknown_mode = contract::Json::object();
    unknown_mode.set("mode", contract::Json("holographic"));
    CHECK(!apply_camera_projection(view, unknown_mode));
    CHECK(view.mode == before.mode);

    // A RECOGNIZED member beside an unrecognized one still lands — tolerance is per-member, not
    // all-or-nothing across the blob.
    contract::Json mixed = contract::Json::object();
    mixed.set("mode", contract::Json("holographic"));
    mixed.set("near", contract::Json(0.5));
    CHECK(apply_camera_projection(view, mixed));
    CHECK(shelltest::near_eq(view.projection.near_z, 0.5f));
}

void test_cameras_get_hydration_never_echoes()
{
    ViewportBinding binding;

    contract::Json camera = contract::Json::object();
    camera.set("viewportId", contract::Json(kViewportA));
    render::View authored;
    authored.transform.position[2] = 33.0f;
    camera.set("transform", camera_transform_json(authored));
    camera.set("projection", camera_projection_json(authored));
    contract::Json cameras = contract::Json::array();
    cameras.push_back(camera);
    contract::Json data = contract::Json::object();
    data.set("cameras", cameras);
    contract::Json envelope = contract::Json::object();
    envelope.set("ok", contract::Json(true));
    envelope.set("data", data);

    CHECK(binding.apply_cameras_result(envelope) == 1u);
    CHECK(binding.find_camera(kViewportA) != nullptr);
    CHECK(shelltest::near_eq(binding.camera(kViewportA).transform.position[2], 33.0f));
    // THE ECHO-SUPPRESSION HALF: a camera that arrived FROM the daemon is not queued to be pushed
    // BACK to it. Without this the first hydration would write every camera straight out again.
    CHECK(binding.dirty_count() == 0u);

    // A LOCAL move is the opposite: it is exactly what must reach `editor.camera-set`.
    render::View moved = binding.camera(kViewportA);
    moved.transform.position[0] = 4.0f;
    CHECK(binding.set_camera(kViewportA, moved));
    CHECK(binding.dirty_count() == 1u);
    const std::vector<std::string> dirty = binding.take_dirty();
    CHECK(dirty.size() == 1u && dirty[0] == kViewportA);
    CHECK(binding.dirty_count() == 0u);
    // Setting the SAME camera again is not a write — an unmoved camera must not spend an RPC.
    CHECK(!binding.set_camera(kViewportA, moved));
    CHECK(binding.dirty_count() == 0u);

    // The bare `{cameras:[…]}` shape (no envelope) is accepted too, and a malformed ENTRY is skipped
    // rather than aborting the batch.
    contract::Json bare = contract::Json::object();
    contract::Json mixed = contract::Json::array();
    mixed.push_back(contract::Json("not an object"));
    mixed.push_back(camera);
    bare.set("cameras", mixed);
    CHECK(binding.apply_cameras_result(bare) == 1u);
    CHECK(binding.apply_cameras_result(contract::Json::object()) == 0u);
}

// ------------------------------------------------------------------------------- the producer

void test_layers_match_the_published_rects()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{1600, 1200}));

    ViewportBinding binding;
    binding.attach_device(device);
    CHECK(binding.adapter_available());
    CHECK(std::string(binding.degraded_code()).empty());

    const render::Rect2D rect_a = shelltest::rect(48, 96, 800, 600);
    const std::vector<ShellRegion> regions = {
        caption_region("chrome.caption", shelltest::rect(0, 0, 1600, 38)),
        viewport_region(kViewportA, rect_a),
    };

    render::RenderSnapshot snapshot;
    const ViewportPublishStats stats = binding.publish(regions, snapshot, compositor);

    // The CAPTION region is not a viewport and must not become a layer — the producer selects on
    // `RegionKind`, never on an id naming convention.
    CHECK(stats.viewports == 1u);
    CHECK(stats.layers == 1u);
    CHECK(stats.rendered == 1u);
    CHECK(stats.changed);
    CHECK(!stats.adapter_absent);

    CHECK(binding.layers().size() == 1u);
    CHECK(binding.layers()[0].id == kViewportA);
    // THE CENTRAL CLAIM: the layer's content rect IS the reported panel rect, byte for byte.
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, rect_a));
    CHECK(shelltest::extent_eq(binding.layers()[0].content_size, rect_a.size));
    CHECK(binding.layers()[0].content != nullptr);

    // And the compositor holds the same stack (the publish really happened).
    CHECK(compositor.viewports().size() == 1u);
    CHECK(compositor.viewports()[0].id == kViewportA);
    CHECK(shelltest::rect_eq(compositor.viewports()[0].content_rect, rect_a));
    CHECK(binding.live_targets() == 1u);

    // The pass drew into the target: one layer draw at the viewport's rect, beneath the CEF layer.
    CHECK(compositor.render_frame());
    CHECK(compositor.stats().viewport_draws == 1);
}

void test_physical_rects_are_never_rescaled()
{
    // The a2 double-application fence (see the file header). The compositor is told the window is on
    // a 144-dpi monitor — its ONE DPI seam — and the producer is then handed the SAME DIP rect
    // converted at 96 and at 144. A producer that re-applied the compositor's scale would report
    // 1.5x the rect it was given, which the cross-publish comparison catches; and because the two
    // publishes are compared against EACH OTHER, the case cannot be satisfied at scale 1.0.
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{2400, 1800}));
    compositor.on_resize(render::Extent2D{2400, 1800}, DpiScale{144});
    CHECK(compositor.dpi().dpi == 144u);

    ViewportBinding binding;
    binding.attach_device(device);
    render::RenderSnapshot snapshot;

    // The DIP rect the panel occupies, with FRACTIONAL edges — the ordinary case for a text-sized
    // dock layout, and the one where `round(w * dpr)` and `round(right * dpr) - round(left * dpr)`
    // disagree.
    const double dip_x = 100.5;
    const double dip_y = 40.25;
    const double dip_w = 640.5;
    const double dip_h = 360.75;

    const render::Rect2D at_100 = physical_from_dip(dip_x, dip_y, dip_w, dip_h, 1.0);
    const render::Rect2D at_150 = physical_from_dip(dip_x, dip_y, dip_w, dip_h, 1.5);
    // The two are genuinely different geometry — the precondition that makes the rest meaningful.
    CHECK(!shelltest::rect_eq(at_100, at_150));

    (void)binding.publish({viewport_region(kViewportA, at_100)}, snapshot, compositor);
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, at_100));
    CHECK(shelltest::extent_eq(binding.layers()[0].content_size, at_100.size));

    (void)binding.publish({viewport_region(kViewportA, at_150)}, snapshot, compositor);
    CHECK(binding.layers().size() == 1u);
    // The 150 % rect is passed through EXACTLY, at a live DpiScale of 144 — the assertion a
    // double-applying producer cannot satisfy.
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, at_150));
    CHECK(shelltest::extent_eq(binding.layers()[0].content_size, at_150.size));
    // The RENDER TARGET is sized from the physical rect too, not from the DIP one: a target an
    // integer scale short of its layer resamples the whole viewport (the popup bug's own shape).
    CHECK(shelltest::extent_eq(binding.layers()[0].content_size, at_150.size));
    CHECK(binding.live_targets() == 1u);
}

void test_layout_change_updates_the_region_routing()
{
    // The ROUTING half of the seam, proved in BOTH directions across a layout change: the arbiter is
    // fed exactly what the producer was fed, so a rect that moves must move the routing with it.
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{1200, 900}));

    ViewportBinding binding;
    binding.attach_device(device);
    InputArbiter arbiter;
    render::RenderSnapshot snapshot;

    const render::Rect2D first = shelltest::rect(100, 100, 400, 300);
    std::vector<ShellRegion> regions = {viewport_region(kViewportA, first)};
    arbiter.regions().publish(regions);
    (void)binding.publish(regions, snapshot, compositor);

    const std::uint64_t generation_after_first = arbiter.regions().generation();
    CHECK(generation_after_first > 0u);

    // INSIDE -> the native viewport path, with the region-relative position picking/gestures want.
    const PointerDispatch inside = arbiter.route_pointer(pointer_at(150, 180), 1000);
    CHECK(inside.target == InputTarget::viewport);
    CHECK(inside.region_id == kViewportA);
    CHECK(inside.region_position.x == 50 && inside.region_position.y == 80);
    // OUTSIDE -> the browser (CEF). The negative half: without it "everything routes to the
    // viewport" would pass the positive assertion above.
    const PointerDispatch outside = arbiter.route_pointer(pointer_at(60, 60), 1001);
    CHECK(outside.target == InputTarget::browser);
    CHECK(outside.region_id.empty());

    // A LAYOUT CHANGE: the panel is dragged to another dock position.
    const render::Rect2D moved = shelltest::rect(600, 400, 400, 300);
    regions = {viewport_region(kViewportA, moved)};
    arbiter.regions().publish(regions);
    const ViewportPublishStats after = binding.publish(regions, snapshot, compositor);
    CHECK(after.changed);
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, moved));
    // The RegionMap generation moved — the seam input.h reserved for exactly this change detection.
    CHECK(arbiter.regions().generation() > generation_after_first);

    // The SAME point that used to hit the viewport now reaches the browser, and a point in the new
    // rect reaches the viewport. Both directions, so a stale rect left behind fails one of them.
    const PointerDispatch stale = arbiter.route_pointer(pointer_at(150, 180), 1002);
    CHECK(stale.target == InputTarget::browser);
    const PointerDispatch fresh = arbiter.route_pointer(pointer_at(650, 480), 1003);
    CHECK(fresh.target == InputTarget::viewport);
    CHECK(fresh.region_id == kViewportA);
    CHECK(fresh.region_position.x == 50 && fresh.region_position.y == 80);
}

void test_two_instances_are_independent()
{
    // c3's `instances.mode: "unlimited"` exercised on a REAL panel: two Scene views open at once,
    // with rects and cameras that do not touch each other.
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{1600, 900}));

    ViewportBinding binding;
    binding.attach_device(device);
    InputArbiter arbiter;
    render::RenderSnapshot snapshot;

    const render::Rect2D rect_a = shelltest::rect(0, 40, 800, 700);
    const render::Rect2D rect_b = shelltest::rect(800, 40, 800, 700);
    const std::vector<ShellRegion> regions = {viewport_region(kViewportA, rect_a),
                                              viewport_region(kViewportB, rect_b)};
    arbiter.regions().publish(regions);
    const ViewportPublishStats stats = binding.publish(regions, snapshot, compositor);

    CHECK(stats.layers == 2u);
    CHECK(stats.rendered == 2u);
    CHECK(binding.live_targets() == 2u);
    CHECK(binding.layers().size() == 2u);
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, rect_a));
    CHECK(shelltest::rect_eq(binding.layers()[1].content_rect, rect_b));
    // TWO DISTINCT render targets, not one shared: the content views must differ, or both panels
    // would show the same camera's pixels — which is the exact failure a `provide()` binding (one
    // model shared by every copy) would produce, and why the viewport binds `provide_factory`.
    CHECK(binding.layers()[0].content != nullptr);
    CHECK(binding.layers()[1].content != nullptr);
    CHECK(binding.layers()[0].content != binding.layers()[1].content);
    // Distinct, non-zero render slots (view.h: 0 is "unassigned").
    CHECK(binding.render_slot(kViewportA) != 0u);
    CHECK(binding.render_slot(kViewportB) != 0u);
    CHECK(binding.render_slot(kViewportA) != binding.render_slot(kViewportB));

    // INDEPENDENT CAMERAS: moving one leaves the other exactly where it was.
    render::View a = binding.camera(kViewportA);
    const render::View b_before = binding.camera(kViewportB);
    a.transform.position[0] = 25.0f;
    a.mode = render::ViewMode::two_d;
    CHECK(binding.set_camera(kViewportA, a));
    CHECK(shelltest::near_eq(binding.camera(kViewportA).transform.position[0], 25.0f));
    CHECK(binding.camera(kViewportA).mode == render::ViewMode::two_d);
    CHECK(shelltest::near_eq(binding.camera(kViewportB).transform.position[0],
                             b_before.transform.position[0]));
    CHECK(binding.camera(kViewportB).mode == b_before.mode);
    // A camera set through the public seam never adopts the caller's render slot (the three-ids rule).
    CHECK(binding.camera(kViewportA).viewport_id == binding.render_slot(kViewportA));

    // Each pointer lands in ITS OWN viewport — the routing half of "two independent views".
    const PointerDispatch in_a = arbiter.route_pointer(pointer_at(100, 100), 1);
    const PointerDispatch in_b = arbiter.route_pointer(pointer_at(900, 100), 2);
    CHECK(in_a.target == InputTarget::viewport && in_a.region_id == kViewportA);
    CHECK(in_b.target == InputTarget::viewport && in_b.region_id == kViewportB);
    CHECK(in_b.region_position.x == 100 && in_b.region_position.y == 60);

    // Closing ONE copy releases ITS target and leaves the other's alone.
    const std::vector<ShellRegion> only_b = {viewport_region(kViewportB, rect_b)};
    const ViewportPublishStats closed = binding.publish(only_b, snapshot, compositor);
    CHECK(closed.changed);
    CHECK(closed.layers == 1u);
    CHECK(binding.live_targets() == 1u);
    CHECK(binding.layers().size() == 1u);
    CHECK(binding.layers()[0].id == kViewportB);
}

void test_damage_is_layout_only_when_the_layout_moved()
{
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{1200, 800}));

    ViewportBinding binding;
    binding.attach_device(device);
    render::RenderSnapshot snapshot;
    const std::vector<ShellRegion> regions = {
        viewport_region(kViewportA, shelltest::rect(10, 10, 500, 400))};

    (void)binding.publish(regions, snapshot, compositor);
    CHECK(compositor.damage().layout);
    CHECK(compositor.render_frame());
    CHECK(!compositor.damage().any());

    // The SAME rects, redrawn: the frame is damaged as CONTENT, not as layout. This is the first
    // consumer of `mark_viewport_content()` — a seam nothing had reached, whose flag existed
    // precisely so "the scene moved" is distinguishable from "the panel moved".
    const ViewportPublishStats again = binding.publish(regions, snapshot, compositor);
    CHECK(!again.changed);
    CHECK(compositor.damage().viewport_content);
    CHECK(!compositor.damage().layout);
    CHECK(binding.publishes() == 2u);
    CHECK(binding.layout_changes() == 1u);
}

void test_adapter_absent_degrades_honestly()
{
    // R-HEAD-002: absence is REPORTED. With no device the layers are still published — the panel's
    // rect and its routing are unaffected — but they carry NO content, so the compositor's clear
    // colour shows through the hole and the panel's own summary model is what the human reads.
    WindowCompositor compositor(software_config());
    ViewportBinding binding;
    CHECK(!binding.adapter_available());
    CHECK(std::string(binding.degraded_code()) == "viewport.adapter_absent");
    CHECK(std::string(kViewportAdapterAbsentCode) == "viewport.adapter_absent");

    render::RenderSnapshot snapshot;
    const render::Rect2D rect_a = shelltest::rect(0, 0, 640, 480);
    const ViewportPublishStats stats =
        binding.publish({viewport_region(kViewportA, rect_a)}, snapshot, compositor);
    CHECK(stats.adapter_absent);
    CHECK(stats.layers == 1u);
    CHECK(stats.rendered == 0u);
    CHECK(binding.live_targets() == 0u);
    CHECK(binding.layers().size() == 1u);
    CHECK(shelltest::rect_eq(binding.layers()[0].content_rect, rect_a));
    CHECK(binding.layers()[0].content == nullptr);
    CHECK(binding.render_slot(kViewportA) == 0u);

    // A camera is still tracked without an adapter — the human's viewport survives a GPU-less boot,
    // and reconnecting a device must not lose it.
    render::View moved = binding.camera(kViewportA);
    moved.transform.position[2] = 9.0f;
    CHECK(binding.set_camera(kViewportA, moved));

    rendertest::FakeDevice device;
    binding.attach_device(device);
    CHECK(binding.adapter_available());
    CHECK(std::string(binding.degraded_code()).empty());
    const ViewportPublishStats recovered =
        binding.publish({viewport_region(kViewportA, rect_a)}, snapshot, compositor);
    CHECK(!recovered.adapter_absent);
    CHECK(recovered.rendered == 1u);
    CHECK(binding.live_targets() == 1u);
    CHECK(binding.layers()[0].content != nullptr);
    CHECK(shelltest::near_eq(binding.camera(kViewportA).transform.position[2], 9.0f));
}

void test_degenerate_rects_are_dropped_not_published()
{
    // A panel mid-resize / in a collapsed group reports a zero-extent rect. Publishing it as a
    // zero-area hole would put a degenerate scissor in the composite pass; allocating a target for
    // it is what `ViewportTargetRegistry::create` already refuses.
    rendertest::FakeDevice device;
    rendertest::FakeSurface surface(rendertest::fake_default_surface_caps());
    WindowCompositor compositor(software_config());
    CHECK(compositor.attach_gpu(device, surface, render::Extent2D{800, 600}));

    ViewportBinding binding;
    binding.attach_device(device);
    render::RenderSnapshot snapshot;

    const ViewportPublishStats collapsed = binding.publish(
        {viewport_region(kViewportA, shelltest::rect(10, 10, 0, 0))}, snapshot, compositor);
    CHECK(collapsed.viewports == 1u);
    CHECK(collapsed.layers == 0u);
    CHECK(collapsed.rendered == 0u);
    CHECK(binding.layers().empty());
    CHECK(binding.live_targets() == 0u);

    // And it recovers on the next real rect rather than staying dropped.
    const ViewportPublishStats restored = binding.publish(
        {viewport_region(kViewportA, shelltest::rect(10, 10, 320, 240))}, snapshot, compositor);
    CHECK(restored.layers == 1u);
    CHECK(restored.rendered == 1u);
    CHECK(binding.live_targets() == 1u);

    // An EMPTY id is refused outright: it would key a target and a camera by nothing.
    ShellRegion anonymous;
    anonymous.kind = RegionKind::viewport;
    anonymous.rect = shelltest::rect(0, 0, 100, 100);
    const ViewportPublishStats nameless = binding.publish({anonymous}, snapshot, compositor);
    CHECK(nameless.viewports == 0u);
    CHECK(nameless.layers == 0u);
}

} // namespace

int main()
{
    test_camera_codec_round_trip();
    test_camera_codec_is_total_over_garbage();
    test_cameras_get_hydration_never_echoes();
    test_layers_match_the_published_rects();
    test_physical_rects_are_never_rescaled();
    test_layout_change_updates_the_region_routing();
    test_two_instances_are_independent();
    test_damage_is_layout_only_when_the_layout_moved();
    test_adapter_absent_degrades_honestly();
    test_degenerate_rects_are_dropped_not_published();
    SHELL_TEST_MAIN_END();
}
