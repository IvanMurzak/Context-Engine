// CPU coverage of the ui-hud golden scene (context/render/ui/hud_scene.h). Compiles the proof header
// under the dev toolchain (catching GCC-tier breaks before the CI render job), pins the reference HUD's
// extracted quads (geometry + composited badge transform + paint order), and asserts the ANALYTIC
// baseline pixels + PPM round-trip — the same numbers the CI GPU readback (lavapipe native / SwiftShader
// web) SSIM-gates against goldens/ui-hud.ppm. Also drives render_golden_ui_hud on the fake backend to
// prove the GpuUiProvider render+readback path runs GPU-free (its quads are no-ops there, like sprite2d).

#include "context/render/ui/hud_scene.h"

#include "context/packages/ui/ui_tree.h"
#include "context/render/golden.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace context::render::ui;
using namespace context::packages::ui;
namespace golden = context::render::golden;

namespace
{

bool rect_eq(const Rect& a, const Rect& b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void test_reference_hud_extracts_expected_quads()
{
    CHECK(ui_hud_target_size() == 256);

    UiTree tree;
    build_reference_hud(tree);
    UiRenderSnapshot snap;
    extract_ui(tree, Rect{0, 0, 256, 256}, snap);

    // Five drawables in paint order: bar background, fill (child, on top), minimap, badge, status.
    CHECK(snap.quads.size() == 5);
    CHECK(rect_eq(snap.quads[0].rect, Rect{16, 16, 120, 20}));
    CHECK((snap.quads[0].color == Color{40, 40, 48, 255}));
    CHECK(rect_eq(snap.quads[1].rect, Rect{18, 18, 80, 16}));
    CHECK((snap.quads[1].color == Color{60, 200, 90, 255}));
    CHECK(rect_eq(snap.quads[2].rect, Rect{176, 176, 64, 64}));
    // The badge's composite transform (scale 1.25 about centre + translate) bakes into its rect.
    CHECK(rect_eq(snap.quads[3].rect, Rect{192, 20, 40, 40}));
    CHECK((snap.quads[3].color == Color{220, 180, 40, 255}));
    CHECK(rect_eq(snap.quads[4].rect, Rect{16, 224, 224, 16}));
}

void test_analytic_baseline_pixels()
{
    golden::GoldenImage img;
    render_ui_hud_reference_cpu(img);
    CHECK(img.width == 256 && img.height == 256);
    CHECK(img.rgba.size() == static_cast<std::size_t>(256) * 256 * 4);

    const std::size_t bpr = 256u * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    auto is = [](const std::uint8_t* px, int r, int g, int b)
    { return px[0] == r && px[1] == g && px[2] == b; };

    CHECK(is(at(4, 4), 15, 18, 26));       // clear (surface background) where no panel covers
    CHECK(is(at(120, 25), 40, 40, 48));    // health-bar background, outside the fill
    CHECK(is(at(50, 25), 60, 200, 90));    // the fill painting OVER the bar background
    CHECK(is(at(208, 208), 40, 80, 160));  // minimap panel
    CHECK(is(at(210, 40), 220, 180, 40));  // the composited (transformed) badge
    CHECK(is(at(100, 230), 30, 30, 36));   // the status bar

    // M7 a8: the shaped-text label rendered — count white (255,255,255) glyph pixels in the label region
    // (mid-left, clear of every panel), which only the cutout text writes. Proves shaped glyphs reached
    // the analytic golden (the same 1-bit mask the GPU cutout draws).
    std::size_t text_ink = 0;
    for (std::uint32_t row = 92; row < 116; ++row)
        for (std::uint32_t col = 18; col < 150; ++col)
            if (is(at(col, row), 255, 255, 255))
                ++text_ink;
    CHECK(text_ink > 20); // "SCORE 1200" at 20px has ample ink
}

void test_analytic_baseline_round_trips_ppm()
{
    golden::GoldenImage img;
    render_ui_hud_reference_cpu(img);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "context-golden-cpu-ui-hud.ppm";
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
    // The minimap pixel (208,208), alpha dropped: offset = header + (208*256 + 208) * 3.
    const std::size_t px = header.size() + (208u * 256u + 208u) * 3u;
    CHECK(static_cast<std::uint8_t>(bytes[px]) == 40);
    CHECK(static_cast<std::uint8_t>(bytes[px + 1]) == 80);
    CHECK(static_cast<std::uint8_t>(bytes[px + 2]) == 160);
    std::filesystem::remove(path);
}

void test_provider_render_path_runs_on_fake_backend()
{
    // The GPU render+readback path runs GPU-free: the fake backend rasterizes only 3-vertex draws, so
    // the HUD quads are Load-pass no-ops and the layer stays the clear color — deterministic evidence
    // the provider's present + read_layer executed (the real rasterization is render-wgpu-ui + goldens/).
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    golden::GoldenImage img;
    CHECK(render_golden_ui_hud(*device, img));
    CHECK(img.width == 256 && img.height == 256);
    CHECK(img.rgba.size() == static_cast<std::size_t>(256) * 256 * 4);
    // The clear color (0.06/0.07/0.10 -> ~15/18/26) fills the layer on the fake backend.
    const std::uint8_t* px = img.rgba.data();
    CHECK(px[0] < 30 && px[2] > px[0] && px[3] == 255);
}

} // namespace

int main()
{
    test_reference_hud_extracts_expected_quads();
    test_analytic_baseline_pixels();
    test_analytic_baseline_round_trips_ppm();
    test_provider_render_path_runs_on_fake_backend();
    RENDER_TEST_MAIN_END();
}
