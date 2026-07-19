// The OSR composite pass (M9 e03; design 03 §4 step 3).
//
// This is the local, GPU-free pin on the three things that must agree for the composite to be
// correct — premultiplied ONE/INV_SRC_ALPHA blending, visible_rect/coded_size UVs, and top-down
// orientation. The SAME expectations are asserted against a real adapter by the `osr` mode of the
// wgpu offscreen proof (ctest render-wgpu-osr-composite); this file is what makes a regression
// visible in seconds on a GPU-less host instead of a CI round-trip.

#include "context/render/present/osr_composite.h"
// osr_scene.h is the GPU proof's fixture + render path. Including it HERE gives the whole header a
// local GCC compile — otherwise it would be built only under CONTEXT_BUILD_RENDER_WGPU, i.e. never
// on this host and never on the default 3-OS build matrix, and a break in it would surface only in
// the render CI job.
#include "context/render/present/osr_scene.h"

#include "render_test.h"

#include <cmath>
#include <string>
#include <vector>

using namespace context::render;
using namespace context::render::present;

namespace
{

bool near_enough(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

std::vector<std::uint8_t> solid_rgba(Extent2D size, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                     std::uint8_t a)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size.width) * size.height * 4u);
    for (std::size_t i = 0; i + 3 < out.size(); i += 4)
    {
        out[i + 0] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }
    return out;
}

std::vector<std::uint8_t> solid_bgra(Extent2D size, std::uint8_t b, std::uint8_t g, std::uint8_t r,
                                     std::uint8_t a)
{
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size.width) * size.height * 4u);
    for (std::size_t i = 0; i + 3 < out.size(); i += 4)
    {
        out[i + 0] = b;
        out[i + 1] = g;
        out[i + 2] = r;
        out[i + 3] = a;
    }
    return out;
}

Rect2D rect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h)
{
    Rect2D r;
    r.origin = {x, y};
    r.size = {w, h};
    return r;
}

// ------------------------------------------------------------------------------------- UV math

void test_uv_is_visible_over_coded()
{
    // The producer allocates in chunks, so coded_size > visible_rect is the NORMAL case; sampling
    // [0,1] would stretch the unused margin across the window.
    const CompositeUv uv = compute_composite_uv(rect(0, 0, 800, 600), Extent2D{1024, 1024});
    CHECK(near_enough(uv.u0, 0.0f));
    CHECK(near_enough(uv.v0, 0.0f));
    CHECK(near_enough(uv.u1, 800.0f / 1024.0f));
    CHECK(near_enough(uv.v1, 600.0f / 1024.0f));
}

void test_uv_offset_visible_rect()
{
    const CompositeUv uv = compute_composite_uv(rect(64, 32, 128, 64), Extent2D{256, 128});
    CHECK(near_enough(uv.u0, 0.25f));
    CHECK(near_enough(uv.v0, 0.25f));
    CHECK(near_enough(uv.u1, 0.75f));
    CHECK(near_enough(uv.v1, 0.75f));
}

void test_uv_edge_cases()
{
    // No visible rect reported yet (first paint) => the whole allocation.
    const CompositeUv none = compute_composite_uv(rect(0, 0, 0, 0), Extent2D{256, 256});
    CHECK(near_enough(none.u1, 1.0f));
    CHECK(near_enough(none.v1, 1.0f));

    // A stale visible rect that overhangs the allocation is CLIPPED, never sampled out of range.
    // The ORIGIN is asserted too: `u1 <= 1` alone would also hold for a regression that discarded
    // the origin and returned the whole [0,1] range.
    const CompositeUv overhang = compute_composite_uv(rect(200, 200, 400, 400), Extent2D{256, 256});
    CHECK(near_enough(overhang.u0, 200.0f / 256.0f));
    CHECK(near_enough(overhang.v0, 200.0f / 256.0f));
    CHECK(near_enough(overhang.u1, 1.0f));
    CHECK(near_enough(overhang.v1, 1.0f));

    // An origin fully OUTSIDE the allocation (a rect a concurrent resize already invalidated) falls
    // back to the whole allocation. Deliberately NOT clip_rect's empty-rect answer: skipping an
    // upload is free, but compositing nothing would blank the editor's UI for that frame.
    const CompositeUv outside = compute_composite_uv(rect(300, 300, 10, 10), Extent2D{256, 256});
    CHECK(near_enough(outside.u0, 0.0f));
    CHECK(near_enough(outside.v0, 0.0f));
    CHECK(near_enough(outside.u1, 1.0f));
    CHECK(near_enough(outside.v1, 1.0f));

    // Degenerate allocation: the default range, not a divide by zero.
    const CompositeUv zero = compute_composite_uv(rect(0, 0, 10, 10), Extent2D{0, 0});
    CHECK(near_enough(zero.u0, 0.0f));
    CHECK(near_enough(zero.u1, 1.0f));
}

// ------------------------------------------------------------------------------ pipeline state

void test_pipeline_uses_premultiplied_blend()
{
    const RenderPipelineDesc desc = make_composite_pipeline_desc(TextureFormat::BGRA8Unorm);
    CHECK(desc.blend.has_value());
    if (desc.blend.has_value())
    {
        // ONE / INV_SRC_ALPHA on BOTH channels. SrcAlpha here would double-darken every antialiased
        // edge in the UI, which is exactly the bug this assertion exists to catch.
        CHECK(desc.blend->color.src == BlendFactor::One);
        CHECK(desc.blend->color.dst == BlendFactor::OneMinusSrcAlpha);
        CHECK(desc.blend->alpha.src == BlendFactor::One);
        CHECK(desc.blend->alpha.dst == BlendFactor::OneMinusSrcAlpha);
    }
    CHECK(desc.color_format == TextureFormat::BGRA8Unorm);
    CHECK(!desc.depth.has_value()); // the UI layer draws over everything
    CHECK(kCompositeVertexCount == 3);

    const std::string wgsl = composite_wgsl();
    CHECK(wgsl.find("vs_main") != std::string::npos);
    CHECK(wgsl.find("fs_main") != std::string::npos);
    CHECK(wgsl.find("textureSample") != std::string::npos);

    // An RGBA target (the offscreen proof) must produce the same state at a different format.
    CHECK(make_composite_pipeline_desc(TextureFormat::RGBA8Unorm).color_format ==
          TextureFormat::RGBA8Unorm);

    // CompositeUv IS the uniform block: four consecutive floats matching WGSL's vec4<f32>, uploaded
    // with no repacking step that could drift out of sync with the shader.
    const CompositeUv u = compute_composite_uv(rect(0, 0, 2, 2), {4, 4});
    CHECK(near_enough(u.u1, 0.5f));
    CHECK(near_enough(u.v1, 0.5f));
    CHECK(sizeof(CompositeUv) == 16u);
    CHECK(wgsl.find("vec4<f32>") != std::string::npos);
}

// --------------------------------------------------------------------------- the blend oracle

void test_opaque_source_replaces_destination()
{
    const Extent2D size{4, 4};
    std::vector<std::uint8_t> dst = solid_rgba(size, 10, 20, 30, 255);
    // BGRA source: B=0x11, G=0x22, R=0x33, A=0xFF.
    const std::vector<std::uint8_t> src = solid_bgra(size, 0x11, 0x22, 0x33, 0xFF);

    composite_reference_cpu(dst, size, src.data(), size, size.width * 4u, rect(0, 0, 4, 4));

    // The destination is RGBA, so the source's BGRA memory must arrive swizzled.
    CHECK(dst[0] == 0x33); // R
    CHECK(dst[1] == 0x22); // G
    CHECK(dst[2] == 0x11); // B
    CHECK(dst[3] == 0xFF); // A
}

void test_fully_transparent_source_leaves_destination()
{
    const Extent2D size{4, 4};
    std::vector<std::uint8_t> dst = solid_rgba(size, 10, 20, 30, 255);
    // Premultiplied + alpha 0 means every colour channel is 0 too.
    const std::vector<std::uint8_t> src = solid_bgra(size, 0, 0, 0, 0);

    composite_reference_cpu(dst, size, src.data(), size, size.width * 4u, rect(0, 0, 4, 4));

    CHECK(dst[0] == 10);
    CHECK(dst[1] == 20);
    CHECK(dst[2] == 30);
    CHECK(dst[3] == 255);
}

void test_half_alpha_folds_premultiplied()
{
    const Extent2D size{2, 2};
    // Destination fully red; source is a 50%-alpha white, PREMULTIPLIED => channels are 128 not 255.
    std::vector<std::uint8_t> dst = solid_rgba(size, 255, 0, 0, 255);
    const std::vector<std::uint8_t> src = solid_bgra(size, 128, 128, 128, 128);

    composite_reference_cpu(dst, size, src.data(), size, size.width * 4u, rect(0, 0, 2, 2));

    // out = src + dst*(1 - 128/255) => R = 128 + 255*127/255 = 255, G = 128 + 0 = 128.
    CHECK(dst[0] == 255);
    CHECK(dst[1] == 128);
    CHECK(dst[2] == 128);
    CHECK(dst[3] == 255);
}

void test_visible_rect_selects_the_sub_image()
{
    // A 4x4 allocation whose bottom-right 2x2 quadrant is the only VISIBLE part. Compositing must
    // pull that quadrant, not the whole allocation.
    const Extent2D coded{4, 4};
    std::vector<std::uint8_t> src(static_cast<std::size_t>(coded.width) * coded.height * 4u, 0u);
    for (std::uint32_t y = 0; y < coded.height; ++y)
    {
        for (std::uint32_t x = 0; x < coded.width; ++x)
        {
            std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * coded.width + x) * 4u;
            const bool in_quadrant = x >= 2 && y >= 2;
            p[0] = 0;                          // B
            p[1] = in_quadrant ? 0xFF : 0x00;  // G
            p[2] = in_quadrant ? 0x00 : 0xFF;  // R
            p[3] = 0xFF;                       // A
        }
    }

    const Extent2D dst_size{2, 2};
    std::vector<std::uint8_t> dst = solid_rgba(dst_size, 0, 0, 0, 255);
    composite_reference_cpu(dst, dst_size, src.data(), coded, coded.width * 4u, rect(2, 2, 2, 2));

    for (std::size_t i = 0; i + 3 < dst.size(); i += 4)
    {
        CHECK(dst[i + 0] == 0x00); // R: the quadrant is green, so red is absent
        CHECK(dst[i + 1] == 0xFF); // G
    }
}

void test_orientation_is_top_down()
{
    // Rows arrive top-down from the producer. A vertical flip would put the top row at the bottom —
    // invisible to a symmetric fixture, so the fixture is deliberately asymmetric.
    const Extent2D size{1, 2};
    std::vector<std::uint8_t> src(8, 0u);
    // Row 0 (top) = red, row 1 (bottom) = green, both opaque, BGRA order.
    src[0] = 0x00;
    src[1] = 0x00;
    src[2] = 0xFF;
    src[3] = 0xFF;
    src[4] = 0x00;
    src[5] = 0xFF;
    src[6] = 0x00;
    src[7] = 0xFF;

    std::vector<std::uint8_t> dst = solid_rgba(size, 0, 0, 0, 255);
    composite_reference_cpu(dst, size, src.data(), size, 4u, rect(0, 0, 1, 2));

    CHECK(dst[0] == 0xFF); // destination row 0 is RED
    CHECK(dst[1] == 0x00);
    CHECK(dst[4] == 0x00); // destination row 1 is GREEN
    CHECK(dst[5] == 0xFF);
}

void test_oracle_refuses_malformed_input()
{
    const Extent2D size{2, 2};
    std::vector<std::uint8_t> dst = solid_rgba(size, 7, 7, 7, 255);
    const std::vector<std::uint8_t> before = dst;

    composite_reference_cpu(dst, size, nullptr, size, 8u, rect(0, 0, 2, 2));
    CHECK(dst == before); // a null source is a no-op, not a crash

    const std::vector<std::uint8_t> src = solid_bgra(size, 1, 2, 3, 255);
    std::vector<std::uint8_t> too_small(4u, 0u);
    composite_reference_cpu(too_small, size, src.data(), size, 8u, rect(0, 0, 2, 2));
    CHECK(too_small.size() == 4u); // an undersized destination is refused, never overrun
}

// ------------------------------------------------------ the GPU proof's fixture (GPU-free half)

void test_reference_frame_is_adversarial()
{
    // The fixture only proves anything if it can DISTINGUISH the two likely mistakes. Assert those
    // properties here, so a well-meant "simplification" of the fixture cannot quietly defang the
    // GPU proof that consumes it.
    const ReferenceOsrFrame frame = build_reference_osr_frame();

    // 1. The allocation is genuinely larger than the visible area, so a [0,1] UV is wrong.
    CHECK(frame.coded_size.width > frame.visible_rect.size.width);
    CHECK(frame.coded_size.height > frame.visible_rect.size.height);
    // 2. The readback row is 256-byte aligned (a WebGPU copy requirement).
    CHECK(frame.target_size.width * 4u % 256u == 0);

    // 3. The visible ORIGIN is non-zero and all four UV components DIFFER. Without both, a shader
    //    that dropped the origin, or swapped u with v, would sample the identical region and the
    //    GPU proof would pass while broken.
    CHECK(frame.visible_rect.origin.x > 0);
    CHECK(frame.visible_rect.origin.y > 0);
    const CompositeUv fixture_uv =
        compute_composite_uv(frame.visible_rect, frame.coded_size);
    CHECK(!near_enough(fixture_uv.u0, fixture_uv.v0));
    CHECK(!near_enough(fixture_uv.u1, fixture_uv.v1));
    CHECK(fixture_uv.u0 > 0.0f);
    CHECK(fixture_uv.v0 > 0.0f);

    // 4. The margin outside the visible rect is magenta — a colour absent from the expectation, so
    //    a wrong UV sub-rect is loud rather than plausible. (0,0) is margin now that the visible
    //    rect is offset; so is the column just past its right edge.
    CHECK(frame.pixels[0] == 0xFF); // B
    CHECK(frame.pixels[1] == 0x00); // G
    CHECK(frame.pixels[2] == 0xFF); // R
    const std::size_t past_right =
        static_cast<std::size_t>(frame.visible_rect.origin.y) * frame.bytes_per_row +
        static_cast<std::size_t>(frame.visible_rect.origin.x + frame.visible_rect.size.width) * 4u;
    CHECK(frame.pixels[past_right + 0] == 0xFF);
    CHECK(frame.pixels[past_right + 2] == 0xFF);

    const std::vector<std::uint8_t> expected = expected_osr_composite(frame);
    CHECK(expected.size() ==
          static_cast<std::size_t>(frame.target_size.width) * frame.target_size.height * 4u);

    const auto at = [&frame](std::uint32_t x, std::uint32_t y)
    { return (static_cast<std::size_t>(y) * frame.target_size.width + x) * 4u; };
    const std::uint32_t last_row = frame.target_size.height - 1u;
    const std::uint32_t last_col = frame.target_size.width - 1u;

    // Top-left quadrant: opaque green replaces the blue backdrop.
    CHECK(expected[at(0, 0) + 0] == 0x00);
    CHECK(expected[at(0, 0) + 1] == 0xFF);
    CHECK(expected[at(0, 0) + 2] == 0x00);

    // Top-right: 50% premultiplied white over opaque blue => (128, 128, 255). An un-premultiplied
    // SrcAlpha blend would land near (64, 64, 191) — far outside the GPU proof's tolerance, which
    // is exactly what makes that mistake detectable.
    const std::size_t right = at(frame.target_size.width / 2u + 1u, 0);
    CHECK(expected[right + 0] == 0x80);
    CHECK(expected[right + 1] == 0x80);
    CHECK(expected[right + 2] == 0xFF);
    CHECK(expected[right + 3] == 0xFF);

    // Bottom-left: opaque red. This is what makes a VERTICAL FLIP detectable — the one transform
    // the vertex stage applies to the UV, and one no fixture with uniform rows can catch.
    CHECK(expected[at(0, last_row) + 0] == 0xFF);
    CHECK(expected[at(0, last_row) + 1] == 0x00);
    CHECK(expected[at(0, last_row) + 2] == 0x00);
    CHECK(expected[at(0, last_row) + 0] != expected[at(0, 0) + 0]); // top and bottom really differ

    // Bottom-right: a fully transparent (all-zero premultiplied) source leaves the pure backdrop,
    // pinning the alpha=0 end of the ONE/INV_SRC_ALPHA fold.
    CHECK(expected[at(last_col, last_row) + 0] == 0x00);
    CHECK(expected[at(last_col, last_row) + 1] == 0x00);
    CHECK(expected[at(last_col, last_row) + 2] == 0xFF);
    CHECK(expected[at(last_col, last_row) + 3] == 0xFF);

    // And magenta appears nowhere in the expectation.
    bool magenta_present = false;
    for (std::size_t i = 0; i + 3 < expected.size(); i += 4)
    {
        if (expected[i + 0] == 0xFF && expected[i + 1] == 0x00 && expected[i + 2] == 0xFF)
        {
            magenta_present = true;
        }
    }
    CHECK(!magenta_present);
}

} // namespace

int main()
{
    test_uv_is_visible_over_coded();
    test_uv_offset_visible_rect();
    test_uv_edge_cases();
    test_pipeline_uses_premultiplied_blend();
    test_opaque_source_replaces_destination();
    test_fully_transparent_source_leaves_destination();
    test_half_alpha_folds_premultiplied();
    test_visible_rect_selects_the_sub_image();
    test_orientation_is_top_down();
    test_oracle_refuses_malformed_input();
    test_reference_frame_is_adversarial();
    RENDER_TEST_MAIN_END();
}
