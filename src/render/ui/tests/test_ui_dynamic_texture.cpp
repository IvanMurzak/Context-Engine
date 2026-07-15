// The M7 a9 dynamic-texture registry (context/render/ui/dynamic_texture.h; lock D4) + the full
// panel-target -> dynamic-texture -> extract chain, driven GPU-free over the fake RHI backend. Pins:
// the registry allocates persistent per-panel targets keyed by handle (0 = unbound); the a6 GpuUiProvider
// generalized to render a UI tree INTO a registry target (world-space form); and a render_world.h UiPanel
// bound to a registry handle is picked up by the L-39 extract, whose handle resolves back to the live RTT.

#include "context/render/ui/dynamic_texture.h"

#include "context/kernel/world.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/extract.h"
#include "context/render/render_world.h"
#include "context/render/ui/provider.h"

#include "render_test.h"
#include "render_test_rhi.h"

using namespace context::render;
using namespace context::render::ui;
using context::kernel::Entity;
using context::kernel::World;

namespace
{

void test_registry_allocates_persistent_panel_targets()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    DynamicTextureRegistry registry(*device);
    CHECK(registry.count() == 0);
    CHECK(!registry.contains(kInvalidDynamicTexture));
    CHECK(registry.get(kInvalidDynamicTexture) == nullptr);

    const DynamicTextureId a = registry.create_panel_target(Extent2D{64, 64});
    const DynamicTextureId b = registry.create_panel_target(Extent2D{128, 32});
    CHECK(a == 1u); // handles start at 1; 0 stays the invalid handle
    CHECK(b == 2u);
    CHECK(registry.count() == 2u);

    // Each handle resolves to a distinct, live, persistent texture at its allocated size.
    CHECK(registry.contains(a));
    CHECK(registry.contains(b));
    CHECK(registry.get(a) != nullptr);
    CHECK(registry.get(b) != nullptr);
    CHECK(registry.get(a) != registry.get(b));
    CHECK(registry.get(a) == registry.get(a)); // persistent — same texture across lookups
    CHECK(registry.size_of(a).width == 64u && registry.size_of(a).height == 64u);
    CHECK(registry.size_of(b).width == 128u && registry.size_of(b).height == 32u);

    // Out-of-range / invalid handles are rejected cleanly.
    CHECK(!registry.contains(3u));
    CHECK(registry.get(3u) == nullptr);
    CHECK(registry.size_of(3u).width == 0u && registry.size_of(3u).height == 0u);
}

void test_provider_renders_a_tree_into_the_registry_target()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    DynamicTextureRegistry registry(*device);
    const DynamicTextureId handle = registry.create_panel_target(Extent2D{64, 64});
    ITexture* target = registry.get(handle);
    CHECK(target != nullptr);

    // The world-space GpuUiProvider form renders into the EXTERNAL registry target (not a self layer).
    GpuUiProvider provider(*device, *target, Extent2D{64, 64}, Color{0.0, 0.0, 0.0, 1.0});
    CHECK(provider.layer_persistent()); // the external target is persistent from construction

    namespace pui = context::packages::ui;
    pui::UiTree tree;
    const pui::NodeId p = tree.create_node(pui::Role::Panel, tree.root());
    tree.set_bounds(p, pui::Rect{8, 8, 32, 32});
    pui::Style s;
    s.background = pui::Color{200, 100, 50, 255};
    tree.set_style(p, s);

    pui::RepaintPlan full;
    full.full_repaint = true;
    provider.present(tree, full);
    CHECK(provider.frames_presented() == 1);
    CHECK(provider.last_was_clear());
    CHECK(provider.last_draw_count() == 1); // the one panel quad

    // read_layer reads back the REGISTRY target (the provider's active layer is the external target).
    std::vector<std::uint8_t> buf;
    CHECK(provider.read_layer(buf));
    CHECK(buf.size() == static_cast<std::size_t>(64) * 64 * 4);
}

void test_panel_target_dynamic_texture_extract_chain()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);

    // panel-target -> dynamic-texture: allocate an RTT and render a panel into it.
    DynamicTextureRegistry registry(*device);
    const DynamicTextureId panel_tex = registry.create_panel_target(Extent2D{64, 64});
    {
        GpuUiProvider provider(*device, *registry.get(panel_tex), Extent2D{64, 64},
                               Color{0.0, 0.0, 0.0, 1.0});
        context::packages::ui::UiTree tree;
        const auto node =
            tree.create_node(context::packages::ui::Role::Panel, tree.root());
        tree.set_bounds(node, context::packages::ui::Rect{0, 0, 64, 64});
        context::packages::ui::RepaintPlan full;
        full.full_repaint = true;
        provider.present(tree, full);
    }

    // -> extract: an entity carrying a Transform + a UiPanel bound to that handle is extracted, and the
    // extracted handle resolves back to the live RTT texture (the full a9 binding chain).
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{{0.5f, 1.0f, -0.5f}, {0, 0, 0, 1}, {1, 1, 1}});
    UiPanel panel;
    panel.texture = panel_tex;
    panel.size[0] = 1.4f;
    panel.size[1] = 1.0f;
    world.add<UiPanel>(e, panel);

    RenderSnapshot snap;
    extract_render_world(world, 1u, snap);
    CHECK(snap.ui_panels.size() == 1u);
    const UiPanelItem& item = snap.ui_panels.front();
    CHECK(item.entity == e);
    CHECK(item.panel.texture == panel_tex);
    CHECK(item.transform.position[0] == 0.5f);
    // The extracted panel's dynamic-texture handle resolves back to the live registry RTT.
    CHECK(registry.contains(item.panel.texture));
    CHECK(registry.get(item.panel.texture) != nullptr);
    CHECK(registry.size_of(item.panel.texture).width == 64u);
}

} // namespace

int main()
{
    test_registry_allocates_persistent_panel_targets();
    test_provider_renders_a_tree_into_the_registry_target();
    test_panel_target_dynamic_texture_extract_chain();
    RENDER_TEST_MAIN_END();
}
