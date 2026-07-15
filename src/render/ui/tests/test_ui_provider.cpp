// The GpuUiProvider (context/render/ui/provider.h) driven END-TO-END over the GPU-free fake RHI backend
// (render_test_rhi.h): the advertised capabilities, and the persistent-layer damage architecture — a
// full repaint CLEARS the layer and draws every quad, a damage repaint LOADs it and draws only the
// minimal overlapping set, and the layer texture is allocated once and reused (persistent).

#include "context/render/ui/provider.h"

#include "context/packages/ui/provider.h" // Capabilities / RepaintPlan
#include "context/packages/ui/ui_tree.h"

#include "render_test.h"
#include "render_test_rhi.h"

using namespace context::render::ui;
using namespace context::packages::ui;

namespace
{

void build_two_panels(UiTree& tree)
{
    const NodeId root = tree.root();
    const NodeId a = tree.create_node(Role::Panel, root);
    tree.set_bounds(a, Rect{8, 8, 16, 16});
    Style as;
    as.background = Color{255, 0, 0, 255};
    tree.set_style(a, as);

    const NodeId b = tree.create_node(Role::Panel, root);
    tree.set_bounds(b, Rect{40, 40, 16, 16});
    Style bs;
    bs.background = Color{0, 255, 0, 255};
    tree.set_style(b, bs);
}

void test_capabilities_are_the_three_advertised()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    GpuUiProvider provider(*device, context::render::Extent2D{64, 64},
                           context::render::Color{0.06, 0.07, 0.10, 1.0});

    const Capabilities caps = provider.capabilities();
    CHECK(caps.gpu_driver);
    CHECK(caps.damage_repaint);
    CHECK(caps.composited_transforms);
    // Text features are a7/a8, false at T1.
    CHECK(!caps.text_shaping);
    CHECK(!caps.bidi);
    CHECK(!caps.ime);
}

void test_present_full_then_damage_then_full()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    GpuUiProvider provider(*device, context::render::Extent2D{64, 64},
                           context::render::Color{0.06, 0.07, 0.10, 1.0});
    UiTree tree;
    build_two_panels(tree);

    // 1) Full repaint: CLEAR + draw both panels. The persistent layer is allocated on this first present.
    RepaintPlan full;
    full.full_repaint = true;
    CHECK(!provider.layer_persistent());
    provider.present(tree, full);
    CHECK(provider.frames_presented() == 1);
    CHECK(provider.last_was_clear());
    CHECK(provider.last_draw_count() == 2);
    CHECK(provider.layer_persistent());

    // 2) Damage repaint over panel A only: LOAD the persistent layer + redraw ONLY the overlapping quad.
    RepaintPlan dmg;
    dmg.regions.push_back(Rect{8, 8, 16, 16});
    provider.present(tree, dmg);
    CHECK(provider.frames_presented() == 2);
    CHECK(!provider.last_was_clear());
    CHECK(provider.last_draw_count() == 1);

    // 3) A no-damage present redraws nothing (still a LOAD — the layer is preserved untouched).
    RepaintPlan none;
    provider.present(tree, none);
    CHECK(provider.frames_presented() == 3);
    CHECK(!provider.last_was_clear());
    CHECK(provider.last_draw_count() == 0);

    // 4) An explicit full repaint clears + draws everything again.
    provider.present(tree, full);
    CHECK(provider.last_was_clear());
    CHECK(provider.last_draw_count() == 2);
}

void test_read_layer_returns_the_surface_sized_buffer()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<context::render::IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    GpuUiProvider provider(*device, context::render::Extent2D{64, 64},
                           context::render::Color{0.06, 0.07, 0.10, 1.0});

    // Nothing presented yet ⇒ no layer to read back.
    std::vector<std::uint8_t> empty;
    CHECK(!provider.read_layer(empty));

    UiTree tree;
    build_two_panels(tree);
    RepaintPlan full;
    full.full_repaint = true;
    provider.present(tree, full);

    std::vector<std::uint8_t> buf;
    CHECK(provider.read_layer(buf));
    CHECK(buf.size() == static_cast<std::size_t>(64) * 64 * 4);
}

} // namespace

int main()
{
    test_capabilities_are_the_three_advertised();
    test_present_full_then_damage_then_full();
    test_read_layer_returns_the_surface_sized_buffer();
    RENDER_TEST_MAIN_END();
}
