// GPU-free coverage of the golden-scene corpus + min-spec bench surface (M4 T7, issue #141;
// R-QA-013): drives golden.h and golden_lit.h end-to-end on the fake RHI backend so the whole
// corpus/bench code path COMPILES and RUNS under the local dev gate with no GPU. The fake backend
// rasterizes only the reference triangle, so triangle3d asserts real pixels; sprite2d and lit3d
// assert the deterministic clear-color frame plus shape/contract behavior (their real
// rasterization is the CI render job's lavapipe leg + the committed goldens/ SSIM gate).

#include "context/render/golden.h"
#include "context/render/lit/golden_lit.h"
#include "context/render/viewport_scene.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace context::render;

namespace
{

std::unique_ptr<IDevice> make_fake_device(rendertest::FakeRhi& rhi)
{
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    return device;
}

void test_triangle3d_renders_and_round_trips_ppm()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = make_fake_device(rhi);

    golden::GoldenImage image;
    CHECK(golden::render_golden_scene(*device, "triangle3d", image));
    CHECK(image.width == offscreen_triangle_size());
    CHECK(image.height == offscreen_triangle_size());
    CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4u);

    const std::size_t row = static_cast<std::size_t>(image.width) * 4u;
    const std::uint8_t* tri = image.rgba.data() + 150u * row + 128u * 4u; // inside the triangle
    CHECK(tri[0] > 200 && tri[1] < 60 && tri[2] < 60);

    // PPM round-trip: header + payload size + a pixel's bytes survive the write.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "context-golden-cpu-triangle3d.ppm";
    CHECK(golden::write_ppm(image, path.string()));
    std::string bytes;
    {
        // Scoped: the stream must be CLOSED before std::filesystem::remove below — Windows blocks
        // deleting a file with an open handle.
        std::ifstream in(path, std::ios::binary);
        CHECK(static_cast<bool>(in));
        std::ostringstream buf;
        buf << in.rdbuf();
        bytes = buf.str();
    }
    const std::string header = "P6\n256 256\n255\n";
    CHECK(bytes.size() == header.size() + static_cast<std::size_t>(256) * 256 * 3);
    CHECK(bytes.compare(0, header.size(), header) == 0);
    // The same interior pixel, alpha dropped: offset = header + (150*256 + 128) * 3.
    const std::size_t px = header.size() + (150u * 256u + 128u) * 3u;
    CHECK(static_cast<std::uint8_t>(bytes[px]) == tri[0]);
    CHECK(static_cast<std::uint8_t>(bytes[px + 1]) == tri[1]);
    std::filesystem::remove(path);
}

void test_sprite2d_shape_and_unknown_scene()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = make_fake_device(rhi);

    golden::GoldenImage image;
    CHECK(golden::render_golden_scene(*device, "sprite2d", image));
    CHECK(image.width == sprite::sprite_target_size());
    CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4u);
    // The fake backend rasterizes only 3-vertex draws, so the sprite quads leave the clear color
    // (0.1/0.2/0.3 -> ~26/51/77) everywhere — deterministic evidence the pass ran.
    const std::uint8_t* px = image.rgba.data();
    CHECK(px[0] < 40 && px[2] > px[0]);

    golden::GoldenImage unused;
    CHECK(!golden::render_golden_scene(*device, "no-such-scene", unused));

    // write_ppm rejects a shape-less image.
    CHECK(!golden::write_ppm(golden::GoldenImage{}, "unused.ppm"));

    // The corpus id table stays in sync with the dispatch above.
    CHECK(golden::kernel_free_scene_ids().size() == 2u);
}

void test_lit3d_golden_and_bench_contract()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = make_fake_device(rhi);

    // lit3d builds the World -> extract -> GPU residency and renders; on the fake backend the
    // scene draw (non-3-vertex) leaves the lit clear color 0.02/0.02/0.04 -> ~5/5/10.
    golden::GoldenImage image;
    CHECK(lit::render_golden_lit3d(*device, image));
    CHECK(image.width == lit::lit_target_size());
    const std::uint8_t* px = image.rgba.data();
    CHECK(px[0] < 12 && px[3] == 255);

    // The bench loop: warmup frames are untimed, samples land 1:1 per measured frame.
    lit::LitBenchResult result;
    CHECK(lit::bench_lit_frames(*device, 256, 128, /*warmup=*/2, /*frames=*/4, result));
    CHECK(result.samples_ms.size() == 4u);
    CHECK(result.width == 256u && result.height == 128u && result.warmup_frames == 2u);
    for (double ms : result.samples_ms)
    {
        CHECK(ms >= 0.0);
    }

    // The JSON contract bench/minspec_floor.py parses.
    const std::string json = lit::bench_result_json(result);
    CHECK(json.find("\"subject\":\"lit3d\"") != std::string::npos);
    CHECK(json.find("\"width\":256") != std::string::npos);
    CHECK(json.find("\"samples_ms\":[") != std::string::npos);
    CHECK(json.find("\"samples_unit\":\"us\"") != std::string::npos);

    // Contract failures: zero frames; a width that breaks the 256-byte readback row alignment.
    lit::LitBenchResult bad;
    CHECK(!lit::bench_lit_frames(*device, 256, 128, 0, 0, bad));
    CHECK(!lit::bench_lit_frames(*device, 100, 128, 0, 1, bad));
}

void test_viewport_composite_shape_and_layers()
{
    // M5-F1 (issue #164): the observer-viewport composite (3D triangle base + 2D sprites overlaid).
    // The fake backend rasterizes only the 3-vertex triangle Clear pass; the sprite quads are
    // Load-pass no-ops there, so on the fake backend the composite == the triangle3d frame (the
    // sprite overlay's real rasterization is the CI render job's lavapipe leg + goldens/viewport.ppm).
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = make_fake_device(rhi);

    golden::GoldenImage image;
    CHECK(render_golden_viewport(*device, image));
    CHECK(image.width == viewport_target_size());
    CHECK(image.height == viewport_target_size());
    CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4u);
    // Same target edge as the triangle + sprite corpus scenes (the layers composite 1:1).
    CHECK(viewport_target_size() == offscreen_triangle_size());
    CHECK(viewport_target_size() == sprite::sprite_target_size());

    const std::size_t row = static_cast<std::size_t>(image.width) * 4u;
    // The 3D triangle base rasterized: an interior triangle pixel is red.
    const std::uint8_t* tri = image.rgba.data() + 150u * row + 128u * 4u;
    CHECK(tri[0] > 200 && tri[1] < 60 && tri[2] < 60);
    // A background corner (neither layer) is the clear color (~26/51/77).
    const std::uint8_t* bg = image.rgba.data();
    CHECK(bg[0] < 40 && bg[2] > bg[0] && bg[3] == 255);

    // The composite round-trips through the PPM writer (the golden interchange format).
    CHECK(golden::write_ppm(image, (std::filesystem::temp_directory_path() /
                                    "context-golden-cpu-viewport.ppm").string()));
    std::filesystem::remove(std::filesystem::temp_directory_path() /
                            "context-golden-cpu-viewport.ppm");
}

} // namespace

int main()
{
    test_triangle3d_renders_and_round_trips_ppm();
    test_sprite2d_shape_and_unknown_scene();
    test_lit3d_golden_and_bench_contract();
    test_viewport_composite_shape_and_layers();
    RENDER_TEST_MAIN_END();
}
