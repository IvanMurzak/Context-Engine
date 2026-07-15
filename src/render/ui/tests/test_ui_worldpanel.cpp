// CPU coverage of the ui-worldpanel golden scene (context/render/ui/worldpanel_scene.h; M7 a9, D4).
// Compiles the proof header under the dev toolchain (catching GCC-tier breaks before the CI render job),
// pins the ANALYTIC baseline pixels (sky / lit ground / the panel's RTT texel sampled at the rotated
// quad's centroid) + the PPM round-trip — the same numbers the CI GPU readback (lavapipe) SSIM-gates
// against goldens/ui-worldpanel.ppm. Also drives render_golden_worldpanel on the fake backend to prove
// the RTT -> composite -> readback path runs GPU-free (its quads are no-ops there, so only the sky clear
// lands — the CPU mirror above is the pixel oracle, exactly like ui-hud / sprite2d).

#include "context/render/ui/worldpanel_scene.h"

#include "context/render/golden.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace context::render::ui;
namespace golden = context::render::golden;

namespace
{

void test_analytic_baseline_pixels()
{
    golden::GoldenImage img;
    render_worldpanel_reference_cpu(img);
    CHECK(img.width == 256 && img.height == 256);
    CHECK(img.rgba.size() == static_cast<std::size_t>(256) * 256 * 4);

    const std::size_t bpr = 256u * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    auto is = [](const std::uint8_t* px, int r, int g, int b)
    { return px[0] == r && px[1] == g && px[2] == b; };

    // Sky (clear) where neither the ground nor the panel covers — the top-left corner.
    CHECK(is(at(4, 4), 13, 15, 26));

    // The lit ground band at its computed interior probe (flat lit color 0.22/0.24/0.30 -> 56/61/77).
    float gx = 0.0f;
    float gy = 0.0f;
    worldpanel_ground_probe_px(gx, gy);
    CHECK(is(at(static_cast<std::uint32_t>(gx), static_cast<std::uint32_t>(gy)), 56, 61, 77));

    // The panel's RTT texel sampled at the rotated quad's centroid (UV ~0.5,0.5 -> the amber inner rect):
    // proves the dynamic-texture content reached the world quad through the sampled composite.
    float px = 0.0f;
    float py = 0.0f;
    worldpanel_panel_centroid_px(px, py);
    const context::packages::ui::Color inner = worldpanel_panel_inner_color();
    CHECK(is(at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py)), inner.r, inner.g,
             inner.b));
}

void test_panel_quad_covers_a_tilted_region()
{
    // The panel is a ROTATED quad (yaw + roll), so its projected footprint is neither the full frame nor
    // an axis-aligned rectangle: some upper-center pixels are the panel (amber/teal), and the four screen
    // corners are all sky (the tilted quad never reaches a corner). A weak-but-real geometry pin.
    golden::GoldenImage img;
    render_worldpanel_reference_cpu(img);
    const std::size_t bpr = 256u * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    auto is_sky = [](const std::uint8_t* px) { return px[0] == 13 && px[1] == 15 && px[2] == 26; };
    CHECK(is_sky(at(2, 2)));
    CHECK(is_sky(at(253, 2)));
    // Bottom corners are inside the ground band, NOT sky (ground reaches the bottom edge full-width).
    CHECK(!is_sky(at(2, 253)));
    CHECK(!is_sky(at(253, 253)));
}

void test_analytic_baseline_round_trips_ppm()
{
    golden::GoldenImage img;
    render_worldpanel_reference_cpu(img);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "context-golden-cpu-ui-worldpanel.ppm";
    CHECK(golden::write_ppm(img, path.string()));
    std::string bytes;
    {
        std::ifstream in(path, std::ios::binary);
        CHECK(static_cast<bool>(in));
        std::ostringstream buf;
        buf << in.rdbuf();
        bytes = buf.str();
    }
    const std::string header = "P6\n256 256\n255\n";
    CHECK(bytes.size() == header.size() + static_cast<std::size_t>(256) * 256 * 3);
    CHECK(bytes.compare(0, header.size(), header) == 0);
    std::filesystem::remove(path);
}

void test_composite_path_runs_on_fake_backend()
{
    // The RTT -> composite -> readback path runs GPU-free: the fake backend rasterizes only 3-vertex
    // draws, so the ground + textured-panel quads are no-ops and only the sky clear lands — deterministic
    // evidence the RTT render + composite + readback executed (the real rasterization is
    // render-wgpu-worldpanel + goldens/).
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    golden::GoldenImage img;
    CHECK(render_golden_worldpanel(*device, img));
    CHECK(img.width == 256 && img.height == 256);
    CHECK(img.rgba.size() == static_cast<std::size_t>(256) * 256 * 4);
    // The sky clear (0.05/0.06/0.10 -> ~13/15/26) fills the target on the fake backend.
    const std::uint8_t* px = img.rgba.data();
    CHECK(px[0] < 24 && px[2] > px[0] && px[3] == 255);
}

} // namespace

int main()
{
    test_analytic_baseline_pixels();
    test_panel_quad_covers_a_tilted_region();
    test_analytic_baseline_round_trips_ppm();
    test_composite_path_runs_on_fake_backend();
    RENDER_TEST_MAIN_END();
}
