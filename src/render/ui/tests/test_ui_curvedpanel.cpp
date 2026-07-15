// CPU coverage of the ui-curvedpanel golden scene (context/render/ui/curvedpanel_scene.h; M7 a10, D4).
// Compiles the proof header under the dev toolchain (catching GCC-tier breaks before the CI render
// job), pins the ANALYTIC baseline pixels (sky / lit ground / the panel's RTT texel sampled on the
// curved surface at the mesh centroid) + the PPM round-trip — the same numbers the CI GPU readback
// (lavapipe) SSIM-gates against goldens/ui-curvedpanel.ppm. Also drives render_golden_curvedpanel on
// the fake backend to prove the RTT -> composite -> readback path runs GPU-free (its quads are no-ops
// there, so only the sky clear lands — the CPU mirror is the pixel oracle, exactly like ui-worldpanel).
//
// `--emit <path>` writes the analytic baseline PPM (the committed goldens/ui-curvedpanel.ppm): the
// GPU-free reviewed-rebaseline command (goldens/README.md), since this dev host has no GPU to render
// the lavapipe golden locally.

#include "context/render/ui/curvedpanel_scene.h"

#include "context/render/golden.h"
#include "context/render/ui/panel_mesh.h"

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
    render_curvedpanel_reference_cpu(img);
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

    // The panel's RTT texel sampled at the curved mesh's projected centroid (UV ~0.5,0.5 -> the amber
    // inner rect): proves the dynamic-texture content reached the CURVED surface through UV sampling.
    float px = 0.0f;
    float py = 0.0f;
    curvedpanel_centroid_px(px, py);
    const context::packages::ui::Color inner = worldpanel_panel_inner_color();
    CHECK(is(at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py)), inner.r, inner.g,
             inner.b));
}

void test_panel_covers_a_curved_region()
{
    // The curved panel floats upper-of-centre: the four screen corners are all NON-panel (sky at the
    // top corners, ground at the bottom), and the panel's teal/amber reaches a band of centre pixels.
    golden::GoldenImage img;
    render_curvedpanel_reference_cpu(img);
    const std::size_t bpr = 256u * 4u;
    auto at = [&](std::uint32_t col, std::uint32_t row)
    { return img.rgba.data() + static_cast<std::size_t>(row) * bpr + static_cast<std::size_t>(col) * 4u; };
    auto is_sky = [](const std::uint8_t* px) { return px[0] == 13 && px[1] == 15 && px[2] == 26; };
    const context::packages::ui::Color bg = worldpanel_panel_bg_color();     // teal
    const context::packages::ui::Color inner = worldpanel_panel_inner_color(); // amber
    auto is_panel = [&](const std::uint8_t* px)
    {
        return (px[0] == bg.r && px[1] == bg.g && px[2] == bg.b) ||
               (px[0] == inner.r && px[1] == inner.g && px[2] == inner.b);
    };

    CHECK(is_sky(at(2, 2)));       // top-left corner is sky (the curved panel never reaches a corner)
    CHECK(is_sky(at(253, 2)));     // top-right corner is sky
    CHECK(!is_sky(at(2, 253)));    // bottom corners are inside the ground band
    CHECK(!is_sky(at(253, 253)));

    // The panel content reaches pixels around its projected centroid.
    float px = 0.0f;
    float py = 0.0f;
    curvedpanel_centroid_px(px, py);
    CHECK(is_panel(at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py))));
}

void test_panel_mesh_seam()
{
    // The a10 panel-mesh data seam (panel_mesh.h): a mesh bound to a dynamic-texture handle is a valid,
    // panel-scoped binding, and its centroid is the geometric centre of the curved surface (x=0 by the
    // arc symmetry, y == the authored centre y).
    const context::packages::ui::PanelMesh mesh = curvedpanel_mesh();
    CHECK(!mesh.empty());
    PanelMeshBinding unbound = bind_panel_mesh(mesh, kInvalidDynamicTexture);
    CHECK(!valid(unbound)); // no texture target -> not renderable
    PanelMeshBinding bound = bind_panel_mesh(mesh, /*texture=*/1u);
    CHECK(valid(bound));
    const context::packages::ui::Vec3 c = panel_mesh_centroid(mesh);
    CHECK(std::fabs(c.x) < 1e-4f);                         // symmetric about x=0
    CHECK(std::fabs(c.y - curvedpanel_center().y) < 1e-4f); // centred at the authored y
}

void test_analytic_baseline_round_trips_ppm()
{
    golden::GoldenImage img;
    render_curvedpanel_reference_cpu(img);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "context-golden-cpu-ui-curvedpanel.ppm";
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
    // draws, so the ground + curved-mesh quads are no-ops and only the sky clear lands — deterministic
    // evidence the RTT render + composite + readback executed (the real rasterization is the CI
    // lavapipe render + goldens/).
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    golden::GoldenImage img;
    CHECK(render_golden_curvedpanel(*device, img));
    CHECK(img.width == 256 && img.height == 256);
    CHECK(img.rgba.size() == static_cast<std::size_t>(256) * 256 * 4);
    const std::uint8_t* px = img.rgba.data();
    CHECK(px[0] < 24 && px[2] > px[0] && px[3] == 255); // the sky clear fills on the fake backend
}

} // namespace

int main(int argc, char** argv)
{
    // `--emit <path>`: write the analytic (GPU-free) baseline PPM — the reviewed-rebaseline command for
    // goldens/ui-curvedpanel.ppm (this dev host has no GPU; the CPU mirror IS the committed baseline).
    if (argc == 3 && std::string(argv[1]) == "--emit")
    {
        golden::GoldenImage img;
        render_curvedpanel_reference_cpu(img);
        if (!golden::write_ppm(img, argv[2]))
        {
            std::fprintf(stderr, "[curvedpanel-emit] FAIL: could not write '%s'\n", argv[2]);
            return 1;
        }
        std::printf("[curvedpanel-emit] wrote %s (%ux%u)\n", argv[2], img.width, img.height);
        return 0;
    }

    test_analytic_baseline_pixels();
    test_panel_covers_a_curved_region();
    test_panel_mesh_seam();
    test_analytic_baseline_round_trips_ppm();
    test_composite_path_runs_on_fake_backend();
    RENDER_TEST_MAIN_END();
}
