// The M6 P7 input DETERMINISM GATE (R-QA-005 / R-SIM-005 / L-54, R-SYS-007) — the sim-path counterpart
// of the physics / animation / particle determinism scenes, for the device-events -> mapped-actions ->
// InputState pipeline.
//
// A fixed schedule of raw device events (keyboard move + mouse fire, plus a mid-run modal UI-capture
// window that BLOCKS gameplay and injects ui actions) is ROUTED through the REAL context_input package
// and fed into a real headless Session through the ordinary injection sink. Every tick's HIERARCHICAL
// canonical state hash (hash_world over the SAME combined sim_components() registry the headless session
// uses — the single InputState the package feeds, never a fork) is accumulated into a trace and
// asserted against a cross-platform GOLDEN. Because the routing is a pure deterministic function of
// (stack, events), the mapped actions are integer, and InputState is integer/fixed-point, the goldens
// are PORTABLE — if any matrix platform routes differently OR folds the InputState differently, THAT
// leg goes red.
//
// Registered as the `determinism-input-scene` ctest, joining the `determinism-*` family the blocking CI
// "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, and the strict-FP
// `deterministic` job (its target is on that job's hand-maintained --target list in ci.yml).
//
// Updating the golden: it changes only when the scene or the router changes ON PURPOSE. Re-derive by
// running this gate — it prints the observed values — then paste them below.

#include "context/packages/input/input_router.h"

#include "context/runtime/session/hash.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"

#include "input_test.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace context::packages::input;
namespace session = context::runtime::session;
namespace kernel = context::kernel;

namespace
{
constexpr int kTicks = 120;
constexpr int kUiOpenTick = 40;  // push the modal (capturing) UI context here
constexpr int kUiCloseTick = 60; // pop it here

// The gameplay + modal-UI context set the scene routes with.
void install_contexts(InputRouter& r)
{
    CHECK(r.install_context(InputContext{"gameplay",
                                         Layer::Gameplay,
                                         false,
                                         {{"keyboard", "D", "move_x"},
                                          {"keyboard", "W", "move_y"},
                                          {"mouse", "MouseLeft", "fire"}}}) == nullptr);
    CHECK(r.install_context(InputContext{
              "pause", Layer::Ui, /*capture=*/true, {{"keyboard", "Escape", "ui_menu"}}}) == nullptr);
    CHECK(r.push_context("gameplay") == nullptr);
}

// The deterministic raw device events for tick `t` (a fixed, integer-only schedule).
std::vector<session::InputEvent> raw_for_tick(int t, bool ui_open)
{
    std::vector<session::InputEvent> raw;
    raw.push_back(session::InputEvent{"keyboard", "D", (t % 7) - 3}); // move_x in [-3, 3]
    raw.push_back(session::InputEvent{"keyboard", "W", (t % 5) - 2}); // move_y in [-2, 2]
    if (t % 3 == 0)
        raw.push_back(session::InputEvent{"mouse", "MouseLeft", 1}); // fire
    if (ui_open && (t % 2 == 0))
        raw.push_back(session::InputEvent{"keyboard", "Escape", 1}); // ui_menu (while modal is up)
    return raw;
}

struct Result
{
    std::uint64_t final_root = 0;
    std::uint64_t trace_fold = 0;
    std::int64_t player_travel = 0; // |px| + |py| of the input-driven player (proves input moved it)
};

[[nodiscard]] Result run_fixture()
{
    InputRouter router;
    install_contexts(router);

    session::Session s(session::SessionConfig{/*seed=*/123, /*tick_hz=*/60, "demo"});
    session::Fnv1a trace;
    bool ui_open = false;
    for (int t = 0; t < kTicks; ++t)
    {
        if (t == kUiOpenTick)
        {
            CHECK(router.push_context("pause") == nullptr); // modal capture ON
            ui_open = true;
        }
        if (t == kUiCloseTick)
        {
            CHECK(router.pop_context() == nullptr); // focus returns to gameplay
            ui_open = false;
        }

        const session::TickInputs routed = router.route(s.sim_tick(), raw_for_tick(t, ui_open));
        for (const session::ActionActivation& a : routed.actions)
            s.inject_action_at(routed.tick, a);

        const session::StepResult sr = s.step(1);
        trace.update_u64(sr.state_hash.root);
    }

    Result r;
    r.final_root = s.state_hash().root;
    r.trace_fold = trace.digest();
    // The demo player is the sole Health-bearing actor; its position is integrated from the mapped
    // move actions, so a non-zero travel proves the routed input actually drove the sim.
    s.world().each<session::Position, session::Health>(
        [&](kernel::Entity, const session::Position& p, const session::Health&)
        {
            r.player_travel = (p.x < 0 ? -p.x : p.x) + (p.y < 0 ? -p.y : p.y);
        });
    return r;
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform
// (Linux-x64 / Win-x64 / macOS-ARM64).
constexpr std::uint64_t kGoldenFinalRoot = 0x2A53561C7E4252E8ULL;
constexpr std::uint64_t kGoldenTraceFold = 0x09629871151A07D9ULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    std::printf("[determinism-input] ticks=%d travel=%lld finalRoot=0x%016llX traceFold=0x%016llX\n",
                kTicks, static_cast<long long>(a.player_travel),
                static_cast<unsigned long long>(a.final_root),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly ------------
    const Result b = run_fixture();
    CHECK(b.final_root == a.final_root);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the scene really is input-active: the routed input moved the player ----------------------
    CHECK(a.player_travel > 0);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 ------
    CHECK(a.final_root == kGoldenFinalRoot);
    CHECK(a.trace_fold == kGoldenTraceFold);

    INPUT_TEST_MAIN_END();
}
