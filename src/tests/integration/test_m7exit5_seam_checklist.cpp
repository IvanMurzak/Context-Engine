// M7 exit criterion 5 — `m7-exit-5-seam-checklist` (design 2026-07-13-m7-runtime-ui / a12-m7-exit) —
// the M7 runtime-UI SEAM CHECKLIST as an executable audit, the milestone-closing mirror of m2-exit-6 /
// m5-exit-3 / m6-exit-5: ONE assertion per M7 seam, exercising the real public surface, so a regression
// that quietly drops a seam turns this milestone gate red. It ADDITIONALLY encodes the two RULED-scope
// seams the design's exit-gate section names (owner rulings c + d):
//
//   1  provider pluggability (R-UI-002 / D1) — null + engine-integrated both implement UiProvider;
//      negotiate_repaint falls back to a full repaint without damage_repaint, incremental with it
//   2  headless authoring loop (R-UI-006 / D6) — build + layout + hit-test + dispatch a tree, no GPU
//   3  RULING (c): shaped-text capability TRUTH — text_shaping + bidi are TRUE on both providers AND
//      the headless shaper actually works (a Latin ligature collapses; an Arabic run resolves RTL) —
//      the same measure() path the a8 ui-test_shaping gate proves
//   4  RULING (d): curved-panel interaction presence — the a10 raycast -> UV -> panel-coords path is
//      registered + green, and the broad-phase-pruned raycaster equals the linear oracle
//   5  single-sink input routing (R-SYS-007 / L-45 / D5) — the a3 UiInputRouter focus lifecycle drives
//      the EXISTING L-45 capture stack (a capturing ui context becomes modal; blur restores gameplay)
//   6  determinism / presentation (D6) — no UI tree operation moves hash_world (UI lives OUTSIDE the World)
//   7  minter discipline — the ui.* fail-closed codes live in the reserved ui domain block (pinned strings)
//
// Runs in the blocking "M7 exit gate" build-job step on all three OS legs.

#include "m7_exit_test.h"

#include "context/kernel/world.h"
#include "context/packages/input/input_router.h"
#include "context/packages/ui/curved_panel.h"
#include "context/packages/ui/errors.h"
#include "context/packages/ui/events.h"
#include "context/packages/ui/input_routing.h"
#include "context/packages/ui/layout.h"
#include "context/packages/ui/null_provider.h"
#include "context/packages/ui/provider.h"
#include "context/packages/ui/raycast_panel.h"
#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/font.h"
#include "context/packages/ui/text/measure.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/ui/provider.h"
#include "context/render/ui/snapshot.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include "render_test_rhi.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using namespace context::packages::ui;
namespace text = context::packages::ui::text;
namespace rui = context::render::ui;
namespace render = context::render;
namespace input = context::packages::input;
namespace session = context::runtime::session;
namespace kernel = context::kernel;
using context::tests::m7::report;

namespace
{
constexpr float kPi = 3.14159265358979323846f;

std::size_t glyph_total(const std::vector<text::ShapedRun>& runs)
{
    std::size_t n = 0;
    for (const text::ShapedRun& r : runs)
        n += r.glyphs.size();
    return n;
}
} // namespace

int main()
{
    // === Seam 1 — provider pluggability (R-UI-002 / D1): both in-repo providers + the fallback table ==
    {
        NullProvider null_provider;
        rendertest::FakeRhi rhi(/*adapter_count=*/1);
        std::unique_ptr<render::IDevice> device = rhi.create_device();
        CHECK(device != nullptr);
        rui::GpuUiProvider gpu_provider(*device, render::Extent2D{128, 32},
                                        render::Color{0.06, 0.07, 0.10, 1.0});

        // The engine-integrated provider advertises the render capabilities; the null one advertises none.
        const Capabilities gpu = gpu_provider.capabilities();
        const Capabilities nul = null_provider.capabilities();
        CHECK(gpu.gpu_driver && gpu.damage_repaint && gpu.composited_transforms);
        CHECK(!nul.gpu_driver && !nul.damage_repaint && !nul.composited_transforms);

        // Negotiation (D1): no damage_repaint => FULL repaint; damage_repaint => incremental over regions.
        DamageList damage;
        damage.add(Rect{10, 10, 20, 20});
        const Rect viewport{0, 0, 128, 32};
        CHECK(negotiate_repaint(nul, damage, viewport).full_repaint);        // null falls back to full
        CHECK(!negotiate_repaint(gpu, damage, viewport).full_repaint);       // gpu repaints the dirty region
    }

    // === Seam 2 — headless authoring loop (R-UI-006 / D6): build -> layout -> hit-test -> dispatch =====
    {
        UiTree tree;
        const NodeId panel = tree.create_node(Role::Panel, tree.root());
        Layout pl;
        pl.size = Vec2{120, 60};
        tree.set_layout(panel, pl);
        const NodeId button = tree.create_node(Role::Button, panel);
        Layout bl;
        bl.size = Vec2{80, 24};
        tree.set_layout(button, bl);
        int fired = 0;
        tree.add_handler(button, EventType::PointerDown, [&fired](Event&) { ++fired; });

        compute_layout(tree, Rect{0, 0, 320, 240});
        const Rect b = tree.node(button)->bounds;
        CHECK(!b.empty()); // layout wrote a real box, no GPU
        const NodeId hit = hit_test(tree, b.x + b.w * 0.5f, b.y + b.h * 0.5f);
        CHECK(hit == button); // top-most hit-test resolves the button
        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = button;
        tree.dispatch(ev);
        CHECK(fired == 1); // target-then-bubble dispatch ran the handler headless
    }

    // === Seam 3 — RULING (c): shaped-text capability TRUTH, backed by a working shaper ================
    {
        // Capability truth: both in-repo providers report text_shaping + bidi TRUE (shaping lives in the
        // HEADLESS text package, so the null provider computes the SAME glyph rects the GPU draws).
        CHECK(NullProvider{}.capabilities().text_shaping);
        CHECK(NullProvider{}.capabilities().bidi);
        rendertest::FakeRhi rhi(/*adapter_count=*/1);
        std::unique_ptr<render::IDevice> device = rhi.create_device();
        CHECK(device != nullptr);
        const Capabilities gpu =
            rui::GpuUiProvider(*device, render::Extent2D{64, 32}, render::Color{0, 0, 0, 1}).capabilities();
        CHECK(gpu.text_shaping && gpu.bidi);

        // ...backed by a PASSING shaper (the a8 measure() path the ui-test_shaping gate proves): a Latin
        // ligature collapses to fewer glyphs than codepoints, and an Arabic run resolves RTL (bidi).
        std::optional<text::FontFace> sans = text::FontFace::from_memory(text::noto_sans_regular());
        std::optional<text::FontFace> arabic = text::FontFace::from_memory(text::noto_sans_arabic_regular());
        CHECK(sans.has_value() && arabic.has_value());
        if (sans && arabic)
        {
            const std::vector<text::ShapedRun> ffi = text::measure(*sans, "ffi", 32.0f);
            CHECK(ffi.size() == 1 && !ffi[0].rtl);
            CHECK(glyph_total(ffi) < 3);   // GSUB collapsed f+f+i -> the ffi ligature (shaping, not cmap)
            CHECK(ffi[0].width > 0.0f);
            const std::vector<text::ShapedRun> ar = text::measure(*arabic, "\xD8\xA8\xD9\x8E", 32.0f);
            CHECK(ar.size() == 1 && ar[0].rtl); // Arabic resolves RTL (SheenBidi UAX #9)
        }
    }

    // === Seam 4 — RULING (d): curved-panel interaction presence (a10 raycast -> UV path, green) ========
    {
        const PanelMesh mesh = build_cylinder_panel_mesh(/*segments=*/16, /*radius=*/1.0f, /*height=*/1.0f,
                                                         /*arc_radians=*/kPi * 0.75f, Vec3{0, 0, 0});
        CHECK(!mesh.empty() && mesh.triangles.size() == 32u);
        const float panel_w = 200.0f;
        const float panel_h = 120.0f;
        const Ray centre{Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};

        const std::optional<Vec2> coords = raycast_panel_pointer(centre, mesh, panel_w, panel_h);
        CHECK(coords.has_value());
        if (coords)
        {
            CHECK(std::fabs(coords->x - 0.5f * panel_w) <= 1.0f);
            CHECK(std::fabs(coords->y - 0.5f * panel_h) <= 1.0f);
        }
        // The broad-phase-pruned picker is REGISTERED + green + identical to the linear oracle.
        const PanelMeshRaycaster raycaster(mesh);
        const PanelRayHit oracle = raycast_panel_mesh(centre, mesh);
        const PanelRayHit pruned = raycaster.raycast(centre);
        CHECK(oracle.hit && pruned.hit);
        CHECK(std::fabs(oracle.uv.x - pruned.uv.x) <= 1e-5f && std::fabs(oracle.uv.y - pruned.uv.y) <= 1e-5f);
        CHECK(raycaster.last_query_candidate_count() < raycaster.triangle_count());
    }

    // === Seam 5 — single-sink input routing (R-SYS-007 / L-45 / D5): the a3 glue drives the L-45 stack =
    {
        UiTree tree;
        input::InputRouter router;
        CHECK(router.install_context(input::InputContext{"gameplay", input::Layer::Gameplay, false,
                                                         {{"keyboard", "D", "move_x"}}}) == nullptr);
        CHECK(router.install_context(input::InputContext{"hud", input::Layer::Ui, /*capture=*/true,
                                                         {{"keyboard", "Escape", "ui_menu"}}}) == nullptr);
        CHECK(router.push_context("gameplay") == nullptr);
        UiInputRouter glue(tree, router, "hud");
        CHECK(!glue.focused());         // the managed ui context is not yet pushed
        CHECK(glue.capturing());        // ...but it IS a capturing (modal) context when active
        CHECK(glue.focus() == nullptr); // focus PUSHES the managed capturing ui context (modal)
        CHECK(glue.focused());          // it is now the active top: unbound gameplay input is swallowed
        CHECK(glue.blur() == nullptr);  // blur POPS it, restoring gameplay routing
        CHECK(!glue.focused());
        // Non-pointer events route through the SAME router (the single sink), not a forked path.
        const session::TickInputs routed =
            glue.route_events(1, {session::InputEvent{"keyboard", "D", 1}});
        CHECK(routed.actions.size() == 1 && routed.actions[0].action == "move_x");
    }

    // === Seam 6 — determinism / presentation (D6): no UI operation moves hash_world ===================
    {
        kernel::World world;
        const session::SimComponentRegistry& reg = session::sim_components();
        const std::uint64_t before = session::hash_world(world, reg).root;

        // A full UI presentation footprint over a tree that lives OUTSIDE the World: build, lay out,
        // extract to draw quads, present through both providers. None of it touches the sim World.
        UiTree hud;
        const NodeId panel = hud.create_node(Role::Panel, hud.root());
        hud.set_bounds(panel, Rect{8, 8, 200, 48});
        Style bg;
        bg.background = Color{0, 0, 0, 160};
        hud.set_style(panel, bg);
        rui::UiRenderSnapshot snap;
        rui::extract_ui(hud, Rect{0, 0, 208, 56}, snap);
        NullProvider null_provider;
        null_provider.present(hud, negotiate_repaint(null_provider.capabilities(), hud.take_damage(),
                                                     Rect{0, 0, 208, 56}));
        CHECK(snap.quads.size() >= 1);
        CHECK(session::hash_world(world, reg).root == before); // hash_world unmoved by ANY UI operation
    }

    // === Seam 7 — minter discipline: the ui.* codes live in the reserved ui domain block ==============
    {
        CHECK(std::string(kSceneNotFoundCode) == "ui.scene_not_found");
        CHECK(std::string(kSceneInvalidCode) == "ui.scene_invalid");
        CHECK(std::string(kNodeNotFoundCode) == "ui.node_not_found");
        CHECK(std::string(kInvalidEventCode) == "ui.invalid_event");
        CHECK(std::string(kAssertionFailedCode) == "ui.assertion_failed");
    }

    return report("m7-exit-5-seam-checklist", "all M7 runtime-UI seams held (incl. rulings c + d)");
}
