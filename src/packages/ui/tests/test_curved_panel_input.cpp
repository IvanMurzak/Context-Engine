// Curved-panel interaction ctest (M7 a10, DoD item 2): a pointer click on a CURVED world-space panel
// routes through the SAME a3 capture path as a flat panel. The ONLY curved-specific step is turning a
// screen ray into panel-space coords via the raycast->UV->panel-coords pipeline (raycast_panel.h,
// broad-phase-pruned through spatial); from there the panel coords feed the EXACT SAME
// UiInputRouter::route_pointer (a3, L-45) a flat panel uses — no forked event path, the single sim
// InputState sink (D5), and UI presence never moves the world hash (D6).
//
// Also pins the broad-phase raycaster: its answer is IDENTICAL to the linear-scan oracle
// (curved_panel.h), and it visits far fewer nodes than the triangle count (spatial pruning works).

#include "context/packages/ui/raycast_panel.h"

#include "context/packages/input/input_router.h"
#include "context/packages/ui/curved_panel.h"
#include "context/packages/ui/input_routing.h"
#include "context/packages/ui/ui_tree.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include "ui_test.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using namespace context::packages::ui;
namespace input = context::packages::input;
namespace session = context::runtime::session;
namespace kernel = context::kernel;

namespace
{

// The panel content surface the curved mesh's UVs address into (the a2 hit_test / route_pointer space).
constexpr float kPanelW = 200.0f;
constexpr float kPanelH = 120.0f;
constexpr float kPi = 3.14159265358979323846f; // M_PI is not a strict-C++20 macro

session::InputState read_input_state(session::Session& s)
{
    session::InputState found;
    s.world().each<session::InputState>([&](kernel::Entity, const session::InputState& st)
                                        { found = st; });
    return found;
}

input::InputContext gameplay_context()
{
    return input::InputContext{"gameplay",
                               input::Layer::Gameplay,
                               false,
                               {{"keyboard", "Space", "fire"}, {"mouse", "MouseLeft", "fire"}}};
}

// A modal (capturing) ui context: unbound gameplay input is swallowed (the R-SYS-007 backdrop).
input::InputContext modal_hud_context()
{
    return input::InputContext{"hud", input::Layer::Ui, /*capture=*/true,
                               {{"keyboard", "Escape", "ui_menu"}}};
}

void inject(session::Session& s, const session::TickInputs& t)
{
    for (const session::ActionActivation& a : t.actions)
        s.inject_action_at(t.tick, a);
}

PanelMesh curved_panel_mesh()
{
    // A convex cylinder-section panel facing +Z (the camera), centred at the origin.
    return build_cylinder_panel_mesh(/*segments=*/16, /*radius=*/1.0f, /*height=*/1.0f,
                                     /*arc_radians=*/kPi * 0.75f, Vec3{0, 0, 0});
}

// An ortho pick ray straight down -Z at a screen/world (x,y) — the golden scene's camera model.
Ray pick_ray(float x, float y)
{
    return Ray{Vec3{x, y, 10.0f}, Vec3{0, 0, -1}};
}

// --- broad-phase raycaster == linear-scan oracle, and it prunes -----------------------------------
void test_raycaster_matches_oracle_and_prunes()
{
    const PanelMesh mesh = curved_panel_mesh();
    const PanelMeshRaycaster raycaster(mesh);
    CHECK(raycaster.triangle_count() == mesh.triangles.size());

    for (const float x : {-0.35f, 0.0f, 0.2f})
    {
        const Ray ray = pick_ray(x, 0.1f);
        const PanelRayHit oracle = raycast_panel_mesh(ray, mesh);
        const PanelRayHit pruned = raycaster.raycast(ray);
        CHECK(oracle.hit == pruned.hit);
        CHECK(oracle.hit);
        // The observable hit (distance + interpolated UV) is IDENTICAL. The triangle INDEX is not
        // asserted: a ray landing on a shared inter-column edge (e.g. x=0) hits two triangles at the
        // same t, and the two scans (index order vs. the spatial query's unspecified candidate order)
        // legally keep different — but equivalent — triangles; the UV/t they yield are the same point.
        CHECK(std::fabs(oracle.t - pruned.t) <= 1e-4f);
        CHECK(std::fabs(oracle.uv.x - pruned.uv.x) <= 1e-5f);
        CHECK(std::fabs(oracle.uv.y - pruned.uv.y) <= 1e-5f);

        // Pruning works: a vertical pick ray touches only the columns straddling x, far below the
        // whole triangle count (16 segments = 32 triangles); the broad phase visits sub-linearly.
        CHECK(raycaster.last_query_candidate_count() < raycaster.triangle_count());
        CHECK(raycaster.last_query_visited_nodes() < mesh.triangles.size() * 2u); // < node_count
    }

    // A miss stays a miss through the pruned path too (no candidate survives the exact test).
    CHECK(!raycaster.raycast(pick_ray(100.0f, 0.0f)).hit);
    CHECK(!raycaster.raycast_pointer(pick_ray(100.0f, 0.0f), kPanelW, kPanelH).has_value());
}

// --- a curved-panel CLICK lands in the sim sink IDENTICALLY to a key-bound action (same a3 path) ---
void test_curved_click_routes_through_a3()
{
    const PanelMesh mesh = curved_panel_mesh();
    const PanelMeshRaycaster raycaster(mesh);

    // Path A: a screen click on the curved panel. The ray hits the panel centre (UV ~0.5,0.5) ->
    // panel coords ~ (0.5*w, 0.5*h); a Button covering the centre emits the SAME "fire" a keybind does.
    input::InputRouter router_a;
    CHECK(router_a.install_context(gameplay_context()) == nullptr);
    CHECK(router_a.install_context(modal_hud_context()) == nullptr);
    CHECK(router_a.push_context("gameplay") == nullptr);

    UiTree tree;
    const NodeId button = tree.create_node(Role::Button, tree.root());
    CHECK(button != kInvalidNode);
    CHECK(tree.set_bounds(button, Rect{70.0f, 40.0f, 60.0f, 40.0f})); // covers (100,60) == centre

    UiInputRouter glue(tree, router_a, "hud");
    tree.add_handler(button, EventType::PointerDown,
                     [&glue](Event&)
                     { glue.emit_action(session::ActionActivation{"fire", "performed", 1}); });
    CHECK(glue.focus() == nullptr);

    // The curved-panel-specific step: screen ray -> raycast -> UV -> panel coords.
    const std::optional<Vec2> coords = raycaster.raycast_pointer(pick_ray(0.0f, 0.0f), kPanelW, kPanelH);
    CHECK(coords.has_value());
    CHECK(std::fabs(coords->x - 0.5f * kPanelW) <= 1.0f);
    CHECK(std::fabs(coords->y - 0.5f * kPanelH) <= 1.0f);

    session::Session s_a(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
    const session::TickInputs via_curved = glue.route_pointer(
        s_a.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, coords->x, coords->y});
    CHECK(via_curved.actions.size() == 1); // the button consumed the pointer and emitted the intent
    CHECK(via_curved.actions[0].action == "fire");
    inject(s_a, via_curved);
    s_a.step(1);

    // Path B: the SAME "fire" from a key-bound source (Space -> fire) — the single-sink comparator.
    input::InputRouter router_b;
    CHECK(router_b.install_context(gameplay_context()) == nullptr);
    CHECK(router_b.push_context("gameplay") == nullptr);
    session::Session s_b(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
    const session::TickInputs via_key =
        router_b.route(s_b.sim_tick(), {session::InputEvent{"keyboard", "Space", 1}});
    CHECK(via_key.actions.size() == 1);
    inject(s_b, via_key);
    s_b.step(1);

    // IDENTICAL landing in the sim InputState sink AND an identical world hash: the curved click took
    // the single a3 action path a keybind takes — no forked event route (D5).
    CHECK(read_input_state(s_a).buttons == 1);
    CHECK(read_input_state(s_a).buttons == read_input_state(s_b).buttons);
    CHECK(session::hash_world(s_a.world(), session::sim_components()).root ==
          session::hash_world(s_b.world(), session::sim_components()).root);
}

// --- a curved-panel MISS is swallowed by the modal, exactly like a flat panel's off-quad miss ------
void test_curved_miss_swallowed_by_modal()
{
    const PanelMesh mesh = curved_panel_mesh();
    const PanelMeshRaycaster raycaster(mesh);

    input::InputRouter router;
    CHECK(router.install_context(gameplay_context()) == nullptr);
    CHECK(router.install_context(modal_hud_context()) == nullptr);
    CHECK(router.push_context("gameplay") == nullptr);

    UiTree tree;
    const NodeId button = tree.create_node(Role::Button, tree.root());
    CHECK(tree.set_bounds(button, Rect{70.0f, 40.0f, 60.0f, 40.0f}));
    UiInputRouter glue(tree, router, "hud");
    tree.add_handler(button, EventType::PointerDown,
                     [&glue](Event&)
                     { glue.emit_action(session::ActionActivation{"fire", "performed", 1}); });
    CHECK(glue.focus() == nullptr);
    CHECK(glue.capturing());

    // A click whose ray MISSES the curved mesh (far to the side): no hit -> no panel coords. The
    // caller routes it as an off-panel pointer (kMiss coords), which the modal ui context swallows —
    // gameplay sees nothing (the SAME modal-backdrop rule a flat panel's off-quad miss follows).
    const std::optional<Vec2> coords = raycaster.raycast_pointer(pick_ray(100.0f, 0.0f), kPanelW, kPanelH);
    CHECK(!coords.has_value());
    session::Session s(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "demo"});
    const session::TickInputs swallowed = glue.route_pointer(
        s.sim_tick(), PointerSample{session::InputEvent{"mouse", "MouseLeft", 1}, -1.0f, -1.0f});
    CHECK(swallowed.actions.empty()); // swallowed by the modal backdrop
    inject(s, swallowed);
    s.step(1);
    CHECK(read_input_state(s).buttons == 0); // gameplay fire never fired
}

} // namespace

int main()
{
    test_raycaster_matches_oracle_and_prunes();
    test_curved_click_routes_through_a3();
    test_curved_miss_swallowed_by_modal();
    UI_TEST_MAIN_END();
}
