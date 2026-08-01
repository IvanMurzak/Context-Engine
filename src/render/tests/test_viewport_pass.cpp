// The M9 e11b viewport render pass (context/render/viewport_pass.h), driven GPU-free over the fake
// RHI backend.
//
// WHAT MAKES THESE ASSERTIONS NON-VACUOUS. The fake backend rasterizes nothing but its own reference
// triangle, so "the target has pixels in it" proves nothing here. What the fake DOES record is every
// uniform payload the pass uploaded, in draw order (FakePassLog::buffer_writes) -- which is exactly
// where the geometry lives, because the pass puts each draw's world->clip matrix in that block. So
// each test decodes the block and asserts the MODEL ORIGIN lands where e11a's own project() puts the
// authored position: an independent computation, an exact value, and one a pass that ignored the
// authored transform (drawing everything at the world origin) cannot produce. The fixtures put every
// entity at a DISTINCT, non-origin position on all three axes for the same reason -- two entities
// sharing a position would make that plant undetectable.

#include "context/render/viewport_pass.h"

#include "context/render/math.h"
#include "context/render/view.h"
#include "context/render/viewport_target.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace context::render;

namespace
{

[[nodiscard]] bool near_enough(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

struct DecodedDraw
{
    Mat4 mvp;
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float shade = 0.0f;
};

// Decode one recorded uniform payload. The size assertion is part of the claim: the block is the
// contract between this pass and its WGSL, and a silently shorter upload would misalign the matrix.
[[nodiscard]] DecodedDraw decode(const std::vector<std::uint8_t>& bytes)
{
    DecodedDraw out;
    CHECK(bytes.size() == sizeof(ViewportDrawUniform));
    if (bytes.size() != sizeof(ViewportDrawUniform))
    {
        return out;
    }
    ViewportDrawUniform block{};
    std::memcpy(&block, bytes.data(), sizeof(block));
    for (std::size_t i = 0; i < 16; ++i)
    {
        out.mvp.m[i] = block.mvp[i];
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        out.color[i] = block.color[i];
    }
    out.shade = block.shade[0];
    return out;
}

// Where a draw's LOCAL point lands in NDC, per the matrix the pass actually uploaded.
[[nodiscard]] Vec3 local_to_ndc(const DecodedDraw& draw, Vec3 local)
{
    return transform_point(draw.mvp, local);
}

// Deliberately a PREDICATE rather than a helper full of CHECKs, and every call site spells out its
// own arguments. CHECK prints the expression text, so folding these comparisons into a void helper
// would make every geometric failure in the file print the SAME line -- and a planting round has to
// attribute each RED to the specific claim it broke. The values are printed on mismatch so the log
// still says which axis and by how much.
[[nodiscard]] bool ndc_equals(Vec3 got, Vec3 expected, float tolerance)
{
    const bool ok = near_enough(got.x, expected.x, tolerance) &&
                    near_enough(got.y, expected.y, tolerance) &&
                    near_enough(got.z, expected.z, tolerance);
    if (!ok)
    {
        std::fprintf(stderr, "  ndc got=(%g, %g, %g) expected=(%g, %g, %g) tol=%g\n",
                     static_cast<double>(got.x), static_cast<double>(got.y),
                     static_cast<double>(got.z), static_cast<double>(expected.x),
                     static_cast<double>(expected.y), static_cast<double>(expected.z),
                     static_cast<double>(tolerance));
    }
    return ok;
}

// The negation, as its own quiet predicate: ndc_equals PRINTS on mismatch, which is the wrong way
// round for an assertion whose passing case IS a mismatch.
[[nodiscard]] bool ndc_differs(Vec3 a, Vec3 b, float tolerance)
{
    return !(near_enough(a.x, b.x, tolerance) && near_enough(a.y, b.y, tolerance) &&
             near_enough(a.z, b.z, tolerance));
}

[[nodiscard]] bool color_equals(const DecodedDraw& draw, Color expected)
{
    const bool ok = near_enough(draw.color[0], static_cast<float>(expected.r), 1e-6f) &&
                    near_enough(draw.color[1], static_cast<float>(expected.g), 1e-6f) &&
                    near_enough(draw.color[2], static_cast<float>(expected.b), 1e-6f) &&
                    near_enough(draw.color[3], static_cast<float>(expected.a), 1e-6f);
    if (!ok)
    {
        std::fprintf(stderr, "  color got=(%g, %g, %g, %g) expected=(%g, %g, %g, %g)\n",
                     static_cast<double>(draw.color[0]), static_cast<double>(draw.color[1]),
                     static_cast<double>(draw.color[2]), static_cast<double>(draw.color[3]),
                     expected.r, expected.g, expected.b, expected.a);
    }
    return ok;
}

// A perspective scene camera pulled back and up, looking roughly down the -Z axis at the origin.
[[nodiscard]] View scene_view_3d()
{
    View view;
    view.mode = ViewMode::three_d;
    view.type = ViewType::scene;
    view.viewport_id = 4u;
    view.transform.position[0] = 1.5f;
    view.transform.position[1] = 6.0f;
    view.transform.position[2] = 11.0f;
    view.projection.fov_y_radians = 1.0471976f;
    view.projection.near_z = 0.1f;
    view.projection.far_z = 500.0f;
    return view;
}

[[nodiscard]] RenderItem make_item(float x, float y, float z, float r, float g, float b)
{
    RenderItem item;
    item.transform.position[0] = x;
    item.transform.position[1] = y;
    item.transform.position[2] = z;
    item.renderable.color[0] = r;
    item.renderable.color[1] = g;
    item.renderable.color[2] = b;
    item.renderable.color[3] = 1.0f;
    return item;
}

// -------------------------------------------------------------------------------------- the grid

void test_the_grid_is_drawn_with_the_right_count_placement_and_palette()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    CHECK(target != nullptr);
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{256, 192};

    ViewportPassConfig config;
    config.grid.half_lines = 4u;
    config.grid.major_every = 2u;
    config.grid.spacing = 1.5f;
    const RenderSnapshot empty; // the grid must draw with no scene at all

    const ViewportFrameStats stats =
        render_viewport_view(device, view, empty, *target_view, size, config);

    // 2 * (2 * 4 + 1) = 18 line boxes: nine parallel to X, then nine parallel to Z.
    CHECK(stats.grid_draws == 18u);
    CHECK(grid_line_count(config.grid) == 18u);
    CHECK(stats.proxy_draws == 0u);
    CHECK(stats.skipped_items == 0u);
    CHECK(stats.rendered);

    // The DEVICE's own record, independent of the function's self-report above.
    const rendertest::FakePassLog& log = device.pass_log();
    CHECK(log.passes == 1);
    CHECK(log.draws == 18);
    CHECK(log.buffer_writes.size() == 18u);

    // Every draw reads its OWN slice of the one uniform buffer. The alignment literal (WebGPU's
    // minUniformBufferOffsetAlignment) is spelled out here rather than read back from
    // kViewportDrawUniformStride, so a change to that constant is CAUGHT instead of tracked. This is
    // the only assertion that can see the bug the offsets exist to prevent: a queue write is ordered
    // BEFORE the pass it feeds, so a pass writing every draw at offset 0 would leave all 18 draws
    // reading the LAST matrix -- and it records byte-identical payloads either way.
    CHECK(log.buffer_write_offsets.size() == 18u);
    for (std::size_t slot = 0; slot < log.buffer_write_offsets.size(); ++slot)
    {
        CHECK(log.buffer_write_offsets[slot] == 256ull * slot);
    }

    // Every expectation below is computed here from the CONFIG, never read back from the subject.
    for (std::int32_t i = -4; i <= 4; ++i)
    {
        const std::size_t slot = static_cast<std::size_t>(i + 4);
        const DecodedDraw along_x = decode(log.buffer_writes[slot]);
        const DecodedDraw along_z = decode(log.buffer_writes[slot + 9u]);

        // Placement: the line parallel to X sits at z = i * spacing on the y = 0 ground plane, and
        // its Z-parallel partner at x = i * spacing.
        const float offset = static_cast<float>(i) * config.grid.spacing;
        CHECK(ndc_equals(local_to_ndc(along_x, Vec3{0.0f, 0.0f, 0.0f}),
                         project(view, size, Vec3{0.0f, 0.0f, offset}), 1e-4f));
        CHECK(ndc_equals(local_to_ndc(along_z, Vec3{0.0f, 0.0f, 0.0f}),
                         project(view, size, Vec3{offset, 0.0f, 0.0f}), 1e-4f));

        // Palette: the index-0 line parallel to X IS the X axis (and vice versa); off the origin,
        // every second line is major here because major_every is 2.
        const Color expect_x = (i == 0) ? config.grid.axis_x
                                        : ((i % 2 == 0) ? config.grid.major : config.grid.minor);
        const Color expect_z = (i == 0) ? config.grid.axis_z
                                        : ((i % 2 == 0) ? config.grid.major : config.grid.minor);
        CHECK(color_equals(along_x, expect_x));
        CHECK(color_equals(along_z, expect_z));

        // The grid is FLAT: only proxies carry the face-shading amount.
        CHECK(along_x.shade == 0.0f);
        CHECK(along_z.shade == 0.0f);
    }

    // The two axis lines are drawn in DIFFERENT colours, so "everything got the same palette slot"
    // cannot pass the loop above by accident.
    CHECK(log.buffer_writes[4] != log.buffer_writes[13]);
}

void test_a_grid_line_spans_the_grid_and_is_thin_across_it()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{256, 256};
    ViewportPassConfig config;
    config.grid.half_lines = 3u;
    config.grid.spacing = 2.0f;
    config.grid.line_width = 0.05f;

    render_viewport_view(device, view, RenderSnapshot{}, *target_view, size, config);

    // The index-0 line parallel to X, i.e. the X axis (slot 3 of 7 in the first half).
    const DecodedDraw axis = decode(device.pass_log().buffer_writes[3]);
    const float half_span = static_cast<float>(config.grid.half_lines) * config.grid.spacing;
    const float half_width = 0.5f * config.grid.line_width;

    // A line box REACHES the far edge of the grid along its own axis...
    CHECK(ndc_equals(local_to_ndc(axis, Vec3{0.5f, 0.0f, 0.0f}),
                     project(view, size, Vec3{half_span, 0.0f, 0.0f}), 1e-4f));
    CHECK(ndc_equals(local_to_ndc(axis, Vec3{-0.5f, 0.0f, 0.0f}),
                     project(view, size, Vec3{-half_span, 0.0f, 0.0f}), 1e-4f));
    // ...and is only line_width thick across it. Asserting the exact thickness (rather than "it is
    // small") is what distinguishes a line box from a full-extent ground quad, which would satisfy
    // the span assertions above perfectly.
    CHECK(ndc_equals(local_to_ndc(axis, Vec3{0.0f, 0.0f, 0.5f}),
                     project(view, size, Vec3{0.0f, 0.0f, half_width}), 1e-4f));
    CHECK(ndc_equals(local_to_ndc(axis, Vec3{0.0f, 0.5f, 0.0f}),
                     project(view, size, Vec3{0.0f, half_width, 0.0f}), 1e-4f));
}

void test_the_grid_can_be_turned_off_without_turning_the_scene_off()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(2.0f, 0.5f, -1.0f, 1.0f, 0.0f, 0.0f));
    snapshot.items.push_back(make_item(-3.0f, 1.5f, 4.0f, 0.0f, 1.0f, 0.0f));

    ViewportPassConfig config;
    config.draw_grid = false; // a Game viewport gets no edit-time overlays (D5)

    const ViewportFrameStats stats = render_viewport_view(device, scene_view_3d(), snapshot,
                                                          *target_view, Extent2D{256, 256}, config);
    // Both halves: no grid AND the scene still drawn. The positive half is what stops this from
    // passing on a pass that drew nothing at all.
    CHECK(stats.grid_draws == 0u);
    CHECK(stats.proxy_draws == 2u);
    CHECK(device.pass_log().draws == 2);
    CHECK(device.pass_log().buffer_writes.size() == 2u);
}

// ----------------------------------------------------------------------------------- the proxies

void test_a_proxy_is_drawn_at_each_renderables_authored_position()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{320, 200};

    // Three DISTINCT, non-origin positions differing on every axis -- see the file header.
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(2.5f, 0.5f, -3.0f, 1.0f, 0.25f, 0.125f));
    snapshot.items.push_back(make_item(-4.0f, 1.5f, 6.25f, 0.2f, 0.8f, 0.4f));
    snapshot.items.push_back(make_item(0.75f, -2.0f, 1.5f, 0.6f, 0.1f, 0.9f));

    ViewportPassConfig config;
    config.draw_grid = false; // isolate the proxy claim from the grid's draws
    config.proxy_shade = 0.4f;

    const ViewportFrameStats stats =
        render_viewport_view(device, view, snapshot, *target_view, size, config);
    CHECK(stats.proxy_draws == 3u);
    CHECK(stats.skipped_items == 0u);
    CHECK(device.pass_log().draws == 3);
    CHECK(device.pass_log().buffer_writes.size() == 3u);

    for (std::size_t i = 0; i < 3; ++i)
    {
        const DecodedDraw draw = decode(device.pass_log().buffer_writes[i]);
        const Transform& transform = snapshot.items[i].transform;
        const Vec3 authored{transform.position[0], transform.position[1], transform.position[2]};

        // THE claim: the proxy's model origin projects to exactly where e11a's project() puts the
        // authored position, through the same View and the same target extent.
        CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.0f, 0.0f, 0.0f}), project(view, size, authored),
                         1e-4f));
        // The tint is the RENDERABLE's own colour, not a constant.
        CHECK(color_equals(draw, Color{static_cast<double>(snapshot.items[i].renderable.color[0]),
                                       static_cast<double>(snapshot.items[i].renderable.color[1]),
                                       static_cast<double>(snapshot.items[i].renderable.color[2]),
                                       1.0}));
        CHECK(near_enough(draw.shade, config.proxy_shade, 1e-6f));
    }

    // No two proxies land in the same PLACE. This must compare the matrices, not the payloads: a
    // payload also carries the item's tint, so `buffer_writes[0] != buffer_writes[1]` stays TRUE on a
    // pass that drew every item at the world origin -- measured, plant P01, where that assertion was
    // the one thing in this test that did NOT redden.
    const DecodedDraw d0 = decode(device.pass_log().buffer_writes[0]);
    const DecodedDraw d1 = decode(device.pass_log().buffer_writes[1]);
    const DecodedDraw d2 = decode(device.pass_log().buffer_writes[2]);
    CHECK(ndc_differs(local_to_ndc(d0, Vec3{}), local_to_ndc(d1, Vec3{}), 1e-4f));
    CHECK(ndc_differs(local_to_ndc(d1, Vec3{}), local_to_ndc(d2, Vec3{}), 1e-4f));
    CHECK(ndc_differs(local_to_ndc(d0, Vec3{}), local_to_ndc(d2, Vec3{}), 1e-4f));
}

void test_a_proxy_is_a_box_of_the_configured_size()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{256, 256};

    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(1.0f, 2.0f, -1.0f, 1.0f, 1.0f, 1.0f));

    ViewportPassConfig config;
    config.draw_grid = false;
    config.proxy_size = 3.0f;

    render_viewport_view(device, view, snapshot, *target_view, size, config);
    const DecodedDraw draw = decode(device.pass_log().buffer_writes[0]);

    // The unit box spans [-0.5, 0.5], so its corners sit half a proxy_size off the authored position
    // on each axis. Pinning the exact EXTENT is what separates "a box was drawn there" from "a
    // degenerate point was drawn there", which the origin assertion alone cannot tell apart.
    const float half = 0.5f * config.proxy_size;
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.5f, 0.5f, 0.5f}),
                     project(view, size, Vec3{1.0f + half, 2.0f + half, -1.0f + half}), 1e-4f));
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{-0.5f, -0.5f, -0.5f}),
                     project(view, size, Vec3{1.0f - half, 2.0f - half, -1.0f - half}), 1e-4f));
}

void test_a_proxy_honours_the_transforms_rotation_and_scale()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{256, 256};

    // A HALF-TURN about Y, whose effect is convention-independent: it maps local +X to world -X and
    // local +Z to world -Z under any consistent right-handed reading, while leaving +Y alone. That
    // matters because the point of this test is that the pass APPLIED the rotation, not which sign
    // convention rotation_from_quaternion uses.
    RenderSnapshot snapshot;
    RenderItem item = make_item(4.0f, 1.0f, -2.0f, 1.0f, 1.0f, 1.0f);
    item.transform.rotation[0] = 0.0f;
    item.transform.rotation[1] = 1.0f;
    item.transform.rotation[2] = 0.0f;
    item.transform.rotation[3] = 0.0f;
    // Three DIFFERENT, non-unit scales. scale[1] was 1.0f in the first cut, which made the +Y
    // assertion below unable to discriminate scale at all -- a plant that dropped the scale entirely
    // left that assertion GREEN while its X and Z siblings reddened. Caught by the plant round, not
    // by reading: an unremarkable-looking fixture VALUE is where this kind of vacuity lives.
    item.transform.scale[0] = 2.0f;
    item.transform.scale[1] = 3.0f;
    item.transform.scale[2] = 0.5f;
    snapshot.items.push_back(item);

    ViewportPassConfig config;
    config.draw_grid = false;
    config.proxy_size = 1.0f;

    render_viewport_view(device, view, snapshot, *target_view, size, config);
    const DecodedDraw draw = decode(device.pass_log().buffer_writes[0]);

    // model = translate * rotate * scale, so local (0.5,0,0) scales to +1.0 on X, flips to -1.0, and
    // lands one metre to the -X side of the authored position.
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.5f, 0.0f, 0.0f}),
                     project(view, size, Vec3{4.0f - 1.0f, 1.0f, -2.0f}), 1e-4f));
    // Local +Z scales to +0.25 and flips to -0.25.
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.0f, 0.0f, 0.5f}),
                     project(view, size, Vec3{4.0f, 1.0f, -2.0f - 0.25f}), 1e-4f));
    // The half-turn leaves +Y alone, so this axis isolates SCALE from rotation: local 0.5 scales to
    // 1.5 and is NOT flipped.
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.0f, 0.5f, 0.0f}),
                     project(view, size, Vec3{4.0f, 1.0f + 1.5f, -2.0f}), 1e-4f));
    // The centre is still the authored position under both.
    CHECK(ndc_equals(local_to_ndc(draw, Vec3{0.0f, 0.0f, 0.0f}),
                     project(view, size, Vec3{4.0f, 1.0f, -2.0f}), 1e-4f));
}

void test_a_two_d_view_projects_the_same_proxies_orthographically()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    View view;
    view.mode = ViewMode::two_d; // the R-2D-001 / L-55 first-class 2D path
    view.type = ViewType::scene;
    view.projection.ortho_half_height = 5.0f;
    view.projection.near_z = 0.0f;
    view.projection.far_z = 100.0f;
    view.transform.position[2] = 10.0f;
    const Extent2D size{320, 160}; // deliberately non-square, so the aspect actually participates

    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(3.0f, -2.0f, 0.0f, 1.0f, 0.0f, 0.0f));
    snapshot.items.push_back(make_item(-6.5f, 4.0f, 0.0f, 0.0f, 0.0f, 1.0f));

    ViewportPassConfig config;
    config.draw_grid = false;

    const ViewportFrameStats stats =
        render_viewport_view(device, view, snapshot, *target_view, size, config);
    CHECK(stats.proxy_draws == 2u);

    for (std::size_t i = 0; i < 2; ++i)
    {
        const DecodedDraw draw = decode(device.pass_log().buffer_writes[i]);
        const Transform& transform = snapshot.items[i].transform;
        CHECK(ndc_equals(
            local_to_ndc(draw, Vec3{0.0f, 0.0f, 0.0f}),
            project(view, size,
                    Vec3{transform.position[0], transform.position[1], transform.position[2]}),
            1e-4f));
    }
    // Under an ORTHOGRAPHIC projection the target's aspect is the only thing separating x from y
    // framing, so pin one NDC value outright: x = 3.0 / (half_height * aspect) = 3 / (5 * 2) = 0.3.
    const DecodedDraw first = decode(device.pass_log().buffer_writes[0]);
    const Vec3 ndc = local_to_ndc(first, Vec3{0.0f, 0.0f, 0.0f});
    CHECK(near_enough(ndc.x, 0.3f, 1e-4f));
    CHECK(near_enough(ndc.y, -0.4f, 1e-4f)); // y = -2.0 / 5.0
}

void test_a_renderable_with_a_non_finite_transform_is_skipped()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    const Extent2D size{256, 256};

    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(2.0f, 1.0f, -1.0f, 1.0f, 0.0f, 0.0f)); // good
    RenderItem nan_position = make_item(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    nan_position.transform.position[1] = std::numeric_limits<float>::quiet_NaN();
    snapshot.items.push_back(nan_position);
    RenderItem infinite_scale = make_item(1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    infinite_scale.transform.scale[2] = std::numeric_limits<float>::infinity();
    snapshot.items.push_back(infinite_scale);
    snapshot.items.push_back(make_item(-5.0f, 3.0f, 2.5f, 1.0f, 1.0f, 0.0f)); // good

    ViewportPassConfig config;
    config.draw_grid = false;

    const ViewportFrameStats stats =
        render_viewport_view(device, view, snapshot, *target_view, size, config);
    CHECK(stats.proxy_draws == 2u);
    CHECK(stats.skipped_items == 2u);
    CHECK(device.pass_log().draws == 2);
    CHECK(device.pass_log().buffer_writes.size() == 2u);

    // POSITIVE: the two SURVIVORS are items 0 and 3 specifically, at their own positions. The counts
    // alone are satisfied just as well by skipping the wrong two.
    CHECK(ndc_equals(local_to_ndc(decode(device.pass_log().buffer_writes[0]), Vec3{}),
                     project(view, size, Vec3{2.0f, 1.0f, -1.0f}), 1e-4f));
    CHECK(ndc_equals(local_to_ndc(decode(device.pass_log().buffer_writes[1]), Vec3{}),
                     project(view, size, Vec3{-5.0f, 3.0f, 2.5f}), 1e-4f));
    // And nothing NaN reached the wire: a poisoned matrix would sail past every comparison above,
    // since every comparison with a NaN is false and CHECK only fires on the negation.
    for (const std::vector<std::uint8_t>& payload : device.pass_log().buffer_writes)
    {
        const DecodedDraw draw = decode(payload);
        for (std::size_t i = 0; i < 16; ++i)
        {
            CHECK(std::isfinite(draw.mvp.m[i]));
        }
    }
}

// -------------------------------------------------------------------------------- the pass itself

void test_the_pass_clears_the_target_even_with_nothing_to_draw()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    ViewportPassConfig config;
    config.draw_grid = false;

    const ViewportFrameStats stats = render_viewport_view(device, scene_view_3d(), RenderSnapshot{},
                                                          *target_view, Extent2D{64, 64}, config);
    // An empty scene shows the clear colour, not last frame's pixels -- so the pass RUNS with zero
    // draws rather than being skipped.
    CHECK(stats.rendered);
    CHECK(stats.grid_draws == 0u);
    CHECK(stats.proxy_draws == 0u);
    CHECK(device.pass_log().passes == 1);
    CHECK(device.pass_log().draws == 0);
    CHECK(device.pass_log().buffer_writes.empty());
}

void test_a_degenerate_target_records_nothing()
{
    rendertest::FakeDevice device;
    std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
    std::unique_ptr<ITextureView> target_view = target->create_view();

    const View view = scene_view_3d();
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f));
    const ViewportPassConfig config;

    for (const Extent2D size : {Extent2D{0, 64}, Extent2D{64, 0}, Extent2D{0, 0}})
    {
        const ViewportFrameStats stats =
            render_viewport_view(device, view, snapshot, *target_view, size, config);
        CHECK(!stats.rendered);
        CHECK(stats.grid_draws == 0u);
        CHECK(stats.proxy_draws == 0u);
    }
    CHECK(device.pass_log().passes == 0);
    CHECK(device.pass_log().draws == 0);
    CHECK(device.pass_log().buffer_writes.empty());

    // The mandatory positive counterpart: the SAME device, view, snapshot and config DO produce a
    // pass at a valid extent, so the absences above are the guard working rather than a fixture that
    // could never have drawn anything.
    const ViewportFrameStats ok =
        render_viewport_view(device, view, snapshot, *target_view, Extent2D{64, 64}, config);
    CHECK(ok.rendered);
    CHECK(ok.proxy_draws == 1u);
    CHECK(device.pass_log().passes == 1);
    CHECK(device.pass_log().draws == static_cast<int>(grid_line_count(config.grid)) + 1);
}

void test_the_depth_attachment_is_wired_only_when_one_is_supplied()
{
    const View view = scene_view_3d();
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f));

    // Without a depth view: the pass still runs (the positive half) and carries no depth attachment.
    {
        rendertest::FakeDevice device;
        std::unique_ptr<ITexture> target = device.create_texture(TextureDesc{});
        std::unique_ptr<ITextureView> target_view = target->create_view();
        ViewportPassConfig config;
        config.draw_grid = false;
        config.depth = nullptr;
        const ViewportFrameStats stats =
            render_viewport_view(device, view, snapshot, *target_view, Extent2D{64, 64}, config);
        CHECK(stats.rendered);
        CHECK(device.pass_log().passes == 1);
        CHECK(device.pass_log().depth_attachments == 0);
        CHECK(!make_viewport_pipeline_desc(TextureFormat::RGBA8Unorm, false).depth.has_value());
    }
    // With one: exactly one depth attachment on the one pass, and the pipeline declares Depth32Float
    // with a Less test. A pass that forgot either draws in submission order and looks nearly right.
    {
        rendertest::FakeDevice device;
        ViewportTargetRegistry registry(device);
        const ViewportTargetId id = registry.acquire_for(1u, Extent2D{64, 64});
        CHECK(id != kInvalidViewportTarget);
        ViewportPassConfig config;
        config.draw_grid = false;
        config.depth = registry.depth_view(id);
        CHECK(config.depth != nullptr);
        const ViewportFrameStats stats = render_viewport_view(
            device, view, snapshot, *registry.color_view(id), registry.size_of(id), config);
        CHECK(stats.rendered);
        CHECK(stats.proxy_draws == 1u);
        CHECK(device.pass_log().passes == 1);
        CHECK(device.pass_log().depth_attachments == 1);

        const RenderPipelineDesc desc = make_viewport_pipeline_desc(TextureFormat::RGBA8Unorm, true);
        CHECK(desc.depth.has_value());
        CHECK(desc.depth->format == TextureFormat::Depth32Float);
        CHECK(desc.depth->depth_write);
        CHECK(desc.depth->depth_compare == CompareFunction::Less);
    }
}

void test_the_pipeline_description_matches_the_shader_contract()
{
    const RenderPipelineDesc desc = make_viewport_pipeline_desc(TextureFormat::BGRA8Unorm, false);
    CHECK(desc.color_format == TextureFormat::BGRA8Unorm); // a swapchain backbuffer, not a registry RT
    CHECK(desc.topology == PrimitiveTopology::TriangleList);
    CHECK(desc.vertex_entry == "vs_main");
    CHECK(desc.fragment_entry == "fs_main");
    CHECK(!desc.blend.has_value()); // opaque replace: proxies and grid lines are not translucent
    CHECK(!desc.wgsl.empty());
    CHECK(desc.wgsl == viewport_pass_wgsl());
    // The box is 12 triangles and the shader's index table must supply every vertex of them.
    CHECK(kViewportBoxVertexCount == 36u);
    // A dynamic uniform offset must be 256-aligned, so the block is padded up to the stride rather
    // than packed at its own 96-byte size.
    CHECK(kViewportDrawUniformStride == 256u);
    CHECK(sizeof(ViewportDrawUniform) <= kViewportDrawUniformStride);
    CHECK(rendertest::mentions(desc.wgsl, "struct Draw"));
    CHECK(rendertest::mentions(desc.wgsl, "@group(0) @binding(0) var<uniform> draw : Draw;"));
    CHECK(rendertest::mentions(desc.wgsl, "mvp : mat4x4<f32>"));
}

void test_the_registry_and_the_pass_compose_into_one_viewport_frame()
{
    // The two halves of e11b wired the way a viewport panel will wire them (e11e does the binding):
    // acquire this viewport's target at the panel's current size, render the view into it, and let a
    // resize reuse the SAME target.
    rendertest::FakeDevice device;
    const int base_live = rendertest::g_live_fake_textures;
    ViewportTargetRegistry registry(device);

    const View view = scene_view_3d();
    RenderSnapshot snapshot;
    snapshot.items.push_back(make_item(2.0f, 0.0f, -2.0f, 1.0f, 0.5f, 0.0f));
    snapshot.items.push_back(make_item(-1.0f, 0.0f, 3.0f, 0.0f, 0.5f, 1.0f));

    ViewportPassConfig config;
    config.grid.half_lines = 2u; // 2 * (2*2+1) = 10 grid lines

    for (std::uint32_t frame = 0; frame < 4u; ++frame)
    {
        const Extent2D size{256u + frame, 144u + frame};
        const ViewportTargetId id = registry.acquire_for(view.viewport_id, size);
        CHECK(id != kInvalidViewportTarget);
        config.depth = registry.depth_view(id);
        const ViewportFrameStats stats = render_viewport_view(
            device, view, snapshot, *registry.color_view(id), registry.size_of(id), config);
        CHECK(stats.rendered);
        CHECK(stats.grid_draws == 10u);
        CHECK(stats.proxy_draws == 2u);
    }

    CHECK(device.pass_log().passes == 4);
    CHECK(device.pass_log().draws == 4 * 12);
    CHECK(device.pass_log().depth_attachments == 4);
    // Four frames at four different sizes still hold ONE target: two live textures, not eight.
    CHECK(registry.live_targets() == 1u);
    CHECK(rendertest::g_live_fake_textures - base_live == 2);
    CHECK(registry.size_of(registry.target_for(view.viewport_id)).width == 259u);
}

} // namespace

int main()
{
    test_the_grid_is_drawn_with_the_right_count_placement_and_palette();
    test_a_grid_line_spans_the_grid_and_is_thin_across_it();
    test_the_grid_can_be_turned_off_without_turning_the_scene_off();
    test_a_proxy_is_drawn_at_each_renderables_authored_position();
    test_a_proxy_is_a_box_of_the_configured_size();
    test_a_proxy_honours_the_transforms_rotation_and_scale();
    test_a_two_d_view_projects_the_same_proxies_orthographically();
    test_a_renderable_with_a_non_finite_transform_is_skipped();
    test_the_pass_clears_the_target_even_with_nothing_to_draw();
    test_a_degenerate_target_records_nothing();
    test_the_depth_attachment_is_wired_only_when_one_is_supplied();
    test_the_pipeline_description_matches_the_shader_contract();
    test_the_registry_and_the_pass_compose_into_one_viewport_frame();
    RENDER_TEST_MAIN_END();
}
