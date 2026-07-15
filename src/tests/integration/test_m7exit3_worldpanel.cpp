// M7 exit criterion 3 — `m7-exit-3-worldpanel` (design 2026-07-13-m7-runtime-ui / a12-m7-exit;
// R-UI-003 / R-SYS-007 / L-16 / L-45, D4/D5/D6, owner ruling d): the roll-3d world-space UI panel
// LOGIC / INTERACTION chain, end to end and headless. Pixels stay with the sibling render jobs (the
// ui-worldpanel / ui-curvedpanel SSIM goldens); THIS gate is the logic chain that turns a screen pick
// ray into a gameplay intent in the ONE sim sink:
//
//   screen ray -> ray-vs-mesh -> barycentric UV -> panel-space coords (a9 FLAT quad + a10 CURVED
//   cylinder mesh, broad-phase-pruned through spatial IDENTICAL to the linear oracle) -> the SAME a3
//   UiInputRouter.route_pointer a flat screen HUD uses (L-45) -> the ONE Session InputState sink.
//
// The authored samples/roll-3d/ui/panel.ui-worldpanel.json is load-bearing: the panel SURFACE size, the
// curved WORLD mesh parameters, and the action button's hit RECT are all read from it, so moving the
// authored button off the panel centre (or resizing the surface) reddens this gate. A UI-routed click
// lands byte-identically to a key-bound "fire" (D5 single sink) and leaves hash_world unmoved (D6 — UI
// is presentation). Runs in the blocking "M7 exit gate" build-job step on all three OS legs.

#include "m7_exit_test.h"

#include "context/editor/contract/json.h"
#include "context/packages/input/input_router.h"
#include "context/packages/ui/curved_panel.h"
#include "context/packages/ui/input_routing.h"
#include "context/packages/ui/raycast_panel.h"
#include "context/packages/ui/ui_tree.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

using namespace context::packages::ui;
namespace contract = context::editor::contract;
namespace input = context::packages::input;
namespace session = context::runtime::session;
namespace kernel = context::kernel;
using context::tests::m7::report;

namespace
{

// The authored world panel, decoded from panel.ui-worldpanel.json (the corpus artifact is load-bearing).
struct AuthoredPanel
{
    bool ok = false;
    float surface_w = 0.0f;
    float surface_h = 0.0f;
    std::uint32_t segments = 0;
    float radius = 0.0f;
    float height = 0.0f;
    float arc = 0.0f;
    Vec3 center;
    Rect button; // the action-button hit rect (surface space)
};

AuthoredPanel load_authored_panel()
{
    AuthoredPanel p;
    const std::string path = std::string(CONTEXT_SAMPLES_DIR) + "/roll-3d/ui/panel.ui-worldpanel.json";
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return p;
    std::ostringstream ss;
    ss << in.rdbuf();
    const contract::Json doc = contract::Json::parse(ss.str());
    if (!doc.is_object() || !doc.contains("surface") || !doc.contains("world") || !doc.contains("panel"))
        return p;

    const contract::Json& surface = doc.at("surface");
    p.surface_w = static_cast<float>(surface.at(0).as_number());
    p.surface_h = static_cast<float>(surface.at(1).as_number());

    const contract::Json& world = doc.at("world");
    p.segments = static_cast<std::uint32_t>(world.at("segments").as_number());
    p.radius = static_cast<float>(world.at("radiusMilli").as_number()) / 1000.0f;
    p.height = static_cast<float>(world.at("heightMilli").as_number()) / 1000.0f;
    p.arc = static_cast<float>(world.at("arcMilliRad").as_number()) / 1000.0f;
    const contract::Json& c = world.at("center");
    p.center = Vec3{static_cast<float>(c.at(0).as_number()), static_cast<float>(c.at(1).as_number()),
                    static_cast<float>(c.at(2).as_number())};

    // Find the action button's authored bounds inside the panel tree.
    const contract::Json& children = doc.at("panel").at("root").at("children");
    for (std::size_t i = 0; i < children.size(); ++i)
    {
        const contract::Json& n = children.at(i);
        if (n.contains("name") && n.at("name").as_string() == "action-button" && n.contains("bounds"))
        {
            const contract::Json& b = n.at("bounds");
            p.button = Rect{static_cast<float>(b.at(0).as_number()), static_cast<float>(b.at(1).as_number()),
                            static_cast<float>(b.at(2).as_number()), static_cast<float>(b.at(3).as_number())};
        }
    }
    p.ok = !p.button.empty() && p.surface_w > 0.0f && p.surface_h > 0.0f && p.segments >= 1;
    return p;
}

// An ortho pick ray straight down -Z at world (x,y) — the golden scene's camera model (a9/a10 tests).
Ray pick_ray(float x, float y)
{
    return Ray{Vec3{x, y, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};
}

// A flat (a9) world-space quad centred on the origin, spanning [-1,1] x [-0.6,0.6] at z=0, UV (0,0)
// top-left .. (1,1) bottom-right — so a centre hit maps to UV (0.5,0.5) exactly like the curved panel.
PanelMesh flat_quad_mesh()
{
    PanelMesh m;
    m.vertices = {
        PanelVertex{Vec3{-1.0f, 0.6f, 0.0f}, Vec2{0.0f, 0.0f}},  // 0 top-left
        PanelVertex{Vec3{1.0f, 0.6f, 0.0f}, Vec2{1.0f, 0.0f}},   // 1 top-right
        PanelVertex{Vec3{-1.0f, -0.6f, 0.0f}, Vec2{0.0f, 1.0f}}, // 2 bottom-left
        PanelVertex{Vec3{1.0f, -0.6f, 0.0f}, Vec2{1.0f, 1.0f}},  // 3 bottom-right
    };
    m.triangles = {{0, 2, 3}, {0, 3, 1}};
    return m;
}

input::InputContext gameplay_context()
{
    return input::InputContext{"gameplay",
                               input::Layer::Gameplay,
                               false,
                               {{"keyboard", "Space", "fire"}, {"mouse", "MouseLeft", "fire"}}};
}

input::InputContext modal_hud_context()
{
    return input::InputContext{"hud", input::Layer::Ui, /*capture=*/true,
                               {{"keyboard", "Escape", "ui_menu"}}};
}

session::InputState read_input_state(session::Session& s)
{
    session::InputState found;
    s.world().each<session::InputState>([&](kernel::Entity, const session::InputState& st)
                                        { found = st; });
    return found;
}

void inject(session::Session& s, const session::TickInputs& t)
{
    for (const session::ActionActivation& a : t.actions)
        s.inject_action_at(t.tick, a);
}

// Build a one-button panel tree (the action button at its authored rect) whose click emits the gameplay
// "fire" intent through the a3 glue — the SAME sink a keybind uses. Returns the button node id.
NodeId build_panel_tree(UiTree& tree, const AuthoredPanel& p, UiInputRouter& glue)
{
    const NodeId button = tree.create_node(Role::Button, tree.root());
    tree.set_bounds(button, p.button);
    tree.add_handler(button, EventType::PointerDown,
                     [&glue](Event&) { glue.emit_action(session::ActionActivation{"fire", "performed", 1}); });
    return button;
}

} // namespace

int main()
{
    const AuthoredPanel panel = load_authored_panel();
    CHECK(panel.ok); // the authored corpus panel parsed (surface + world mesh + action-button rect)
    if (!panel.ok)
        return report("m7-exit-3-worldpanel", "authored panel failed to load");

    const PanelMesh curved =
        build_cylinder_panel_mesh(panel.segments, panel.radius, panel.height, panel.arc, panel.center);
    const PanelMesh flat = flat_quad_mesh();
    CHECK(!curved.empty());
    CHECK(curved.triangles.size() == panel.segments * 2u);

    // --- the raycast->UV->panel-coords pipeline lands on the panel centre (a9 flat AND a10 curved) ----
    for (const PanelMesh* mesh : {&flat, &curved})
    {
        const std::optional<Vec2> coords =
            raycast_panel_pointer(pick_ray(panel.center.x, panel.center.y), *mesh, panel.surface_w,
                                  panel.surface_h);
        CHECK(coords.has_value());
        if (coords.has_value())
        {
            CHECK(std::fabs(coords->x - 0.5f * panel.surface_w) <= 1.0f);
            CHECK(std::fabs(coords->y - 0.5f * panel.surface_h) <= 1.0f);
            CHECK(panel.button.contains(coords->x, coords->y)); // the centre hit lands on the button rect
        }
    }

    // --- the broad-phase-pruned raycaster (a10) is IDENTICAL to the linear-scan oracle, and it prunes --
    {
        const PanelMeshRaycaster raycaster(curved);
        CHECK(raycaster.triangle_count() == curved.triangles.size());
        const Ray ray = pick_ray(panel.center.x + 0.2f, panel.center.y + 0.1f);
        const PanelRayHit oracle = raycast_panel_mesh(ray, curved);
        const PanelRayHit pruned = raycaster.raycast(ray);
        CHECK(oracle.hit && pruned.hit);
        CHECK(std::fabs(oracle.t - pruned.t) <= 1e-4f);
        CHECK(std::fabs(oracle.uv.x - pruned.uv.x) <= 1e-5f);
        CHECK(std::fabs(oracle.uv.y - pruned.uv.y) <= 1e-5f);
        CHECK(raycaster.last_query_candidate_count() < raycaster.triangle_count()); // sub-linear pruning
        CHECK(!raycaster.raycast(pick_ray(100.0f, 0.0f)).hit);                       // a clean miss stays a miss
    }

    // --- a curved-panel CLICK lands in the sim sink IDENTICALLY to a key-bound "fire" (single a3 path) -
    {
        const PanelMeshRaycaster raycaster(curved);

        // Path A: screen click on the curved world panel -> raycast -> UV -> panel coords -> a3 route.
        input::InputRouter router_a;
        CHECK(router_a.install_context(gameplay_context()) == nullptr);
        CHECK(router_a.install_context(modal_hud_context()) == nullptr);
        CHECK(router_a.push_context("gameplay") == nullptr);
        UiTree tree;
        UiInputRouter glue(tree, router_a, "hud");
        build_panel_tree(tree, panel, glue);
        CHECK(glue.focus() == nullptr);

        const std::optional<Vec2> coords =
            raycaster.raycast_pointer(pick_ray(panel.center.x, panel.center.y), panel.surface_w,
                                      panel.surface_h);
        CHECK(coords.has_value());
        session::Session s_a(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
        const session::TickInputs via_panel = glue.route_pointer(
            s_a.sim_tick(),
            PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, coords ? coords->x : -1.0f,
                          coords ? coords->y : -1.0f});
        CHECK(via_panel.actions.size() == 1);
        if (via_panel.actions.size() == 1)
            CHECK(via_panel.actions[0].action == "fire");
        inject(s_a, via_panel);
        s_a.step(1);

        // Path B: the SAME "fire" from a key-bound source — the single-sink comparator.
        input::InputRouter router_b;
        CHECK(router_b.install_context(gameplay_context()) == nullptr);
        CHECK(router_b.push_context("gameplay") == nullptr);
        session::Session s_b(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
        const session::TickInputs via_key =
            router_b.route(s_b.sim_tick(), {session::InputEvent{"keyboard", "Space", 1}});
        inject(s_b, via_key);
        s_b.step(1);

        // Identical landing in the sim InputState sink AND an identical world hash: the world-panel click
        // took the single a3 action path a keybind takes (D5), and UI presence never moved sim state (D6).
        CHECK(read_input_state(s_a).buttons == 1);
        CHECK(read_input_state(s_a).buttons == read_input_state(s_b).buttons);
        CHECK(session::hash_world(s_a.world(), session::sim_components()).root ==
              session::hash_world(s_b.world(), session::sim_components()).root);
    }

    // --- a world-panel MISS under a modal is swallowed, exactly like a flat panel's off-quad miss ------
    {
        const PanelMeshRaycaster raycaster(curved);
        input::InputRouter router;
        CHECK(router.install_context(gameplay_context()) == nullptr);
        CHECK(router.install_context(modal_hud_context()) == nullptr);
        CHECK(router.push_context("gameplay") == nullptr);
        UiTree tree;
        UiInputRouter glue(tree, router, "hud");
        build_panel_tree(tree, panel, glue);
        CHECK(glue.focus() == nullptr);
        CHECK(glue.capturing());

        const std::optional<Vec2> miss =
            raycaster.raycast_pointer(pick_ray(100.0f, 0.0f), panel.surface_w, panel.surface_h);
        CHECK(!miss.has_value());
        session::Session s(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
        const session::TickInputs swallowed = glue.route_pointer(
            s.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, -1.0f, -1.0f});
        CHECK(swallowed.actions.empty()); // swallowed by the modal backdrop (no gameplay leak)
        inject(s, swallowed);
        s.step(1);
        CHECK(read_input_state(s).buttons == 0);
    }

    return report("m7-exit-3-worldpanel",
                  "the roll-3d world-space panel logic chain (raycast->UV->a3 sink) held, flat + curved");
}
