// M7 exit criterion 4 — `m7-exit-4-determinism-presentation` (design 2026-07-13-m7-runtime-ui /
// a12-m7-exit; R-SIM-005 / R-QA-005, lock D6): the sim state hash is BIT-IDENTICAL whether the runtime
// UI is ABSENT, presented through the NULL provider, or presented through the engine-integrated GPU
// provider. This pins D6 forever: UI is presentation — reading sim state to drive a HUD, extracting the
// retained tree, and presenting it (null OR GPU) must NEVER perturb the deterministic simulation.
//
// The SAME sample-game session (same seed, same fixed steps) is run three ways: with no UI work in the
// inter-tick service point, with a HUD presented through NullProvider each inter-tick, and with a HUD
// presented through GpuUiProvider (over the GPU-free fake RHI — the a11 capability-matrix precedent).
// The per-tick AND final hash_world are compared: any divergence between the three legs reds the gate.
// The GPU leg's provider is REAL (context::render::ui::GpuUiProvider), driven headlessly — pixels stay
// with the sibling render jobs. Runs in the blocking "M7 exit gate" build-job step on all three OS legs.

#include "m7_exit_test.h"

#include "sample_games.h"

#include "context/packages/ui/null_provider.h"
#include "context/packages/ui/provider.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/ui/provider.h"
#include "context/render/ui/snapshot.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/sim_component.h"
#include "context/runtime/session/state_hash.h"

#include "render_test_rhi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace games = context::tests::games;
namespace session = context::runtime::session;
namespace ui = context::packages::ui;
namespace rui = context::render::ui;
namespace render = context::render;
using context::tests::m7::report;

namespace
{

constexpr int kTicks = 90;

// How the inter-tick presentation runs (or does not) — the three legs of the D6 proof.
enum class Presentation
{
    Absent, // no UI work at all
    Null,   // present the HUD through the null / headless provider
    Gpu     // present the HUD through the engine-integrated GPU provider (fake RHI, headless)
};

// A tiny HUD the presentation legs drive: a panel + a data-bound-style label whose text tracks a sim
// read (score). Built once; the inter-tick hook updates + presents it. Presentation only (D6). Returns
// the label node id (the sim-read target).
ui::NodeId build_hud(ui::UiTree& tree)
{
    const ui::NodeId panel = tree.create_node(ui::Role::Panel, tree.root());
    tree.set_bounds(panel, ui::Rect{8, 8, 200, 48});
    ui::Style bg;
    bg.background = ui::Color{0, 0, 0, 160};
    tree.set_style(panel, bg);
    const ui::NodeId label = tree.create_node(ui::Role::Label, panel);
    tree.set_bounds(label, ui::Rect{12, 12, 180, 20});
    ui::Style fg;
    fg.background = ui::Color{40, 40, 48, 255};
    tree.set_style(label, fg);
    return label;
}

// Run the platformer sample game for kTicks fixed ticks under `mode`, returning the per-tick hash_world
// sequence (the milestone digest law). Under Null/Gpu, each inter-tick reads sim state (read-only) into a
// HUD, extracts it, and presents it through the provider — the exact D6 presentation footprint.
std::vector<std::uint64_t> run(Presentation mode, std::uint64_t& frames_presented)
{
    session::Session s(session::SessionConfig{/*seed=*/7, /*tick_hz=*/60, "platformer-2d"});

    ui::UiTree hud;
    const ui::NodeId hud_label = build_hud(hud);
    ui::NullProvider null_provider;

    // The real GPU provider over the GPU-free fake RHI (headless) — advertises gpu_driver, presents no
    // pixels we assert here (pixels are the sibling render jobs). Kept alive for the whole run.
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<render::IDevice> device = rhi.create_device();
    std::unique_ptr<rui::GpuUiProvider> gpu_provider;
    if (mode == Presentation::Gpu && device)
        gpu_provider =
            std::make_unique<rui::GpuUiProvider>(*device, render::Extent2D{208, 56},
                                                 render::Color{0.06, 0.07, 0.10, 1.0});

    rui::UiRenderSnapshot snap;
    if (mode != Presentation::Absent)
    {
        s.set_inter_tick_hook(
            [&](std::uint64_t)
            {
                // sim -> UI (READ-ONLY): read the game's live score-ish state to drive the HUD label.
                const games::PlatformerGame g = games::read_platformer_game(s.world());
                hud.set_text(hud_label, std::to_string(static_cast<long long>(g.jumps + g.coin_collected)));
                // Extract the retained tree into draw quads (the a6 read-only observer) + present.
                rui::extract_ui(hud, ui::Rect{0, 0, 208, 56}, snap);
                const ui::RepaintPlan plan = ui::negotiate_repaint(
                    mode == Presentation::Gpu ? gpu_provider->capabilities() : null_provider.capabilities(),
                    hud.take_damage(), ui::Rect{0, 0, 208, 56});
                if (mode == Presentation::Gpu && gpu_provider)
                    gpu_provider->present(hud, plan);
                else
                    null_provider.present(hud, plan);
            });
    }

    std::vector<std::uint64_t> hashes;
    hashes.reserve(kTicks);
    for (int t = 0; t < kTicks; ++t)
    {
        s.step(1); // gravity + collisions evolve the sim deterministically; the hook presents mid-gap
        hashes.push_back(session::hash_world(s.world(), session::sim_components()).root);
    }
    frames_presented = null_provider.frames_presented();
    return hashes;
}

} // namespace

int main()
{
    games::register_platformer2d_scenario(CONTEXT_SAMPLES_DIR);

    std::uint64_t null_frames = 0;
    std::uint64_t gpu_frames = 0;
    std::uint64_t absent_frames = 0;
    const std::vector<std::uint64_t> absent = run(Presentation::Absent, absent_frames);
    const std::vector<std::uint64_t> with_null = run(Presentation::Null, null_frames);
    const std::vector<std::uint64_t> with_gpu = run(Presentation::Gpu, gpu_frames);

    // The sim genuinely evolved (a constant-hash run would prove nothing).
    CHECK(absent.size() == static_cast<std::size_t>(kTicks));
    CHECK(absent.front() != absent.back());

    // The presentation legs actually presented (fail-closed: an un-run hook proves nothing).
    CHECK(null_frames == static_cast<std::uint64_t>(kTicks));
    CHECK(gpu_frames == 0); // the GPU leg used the GPU provider, not the null one

    // D6: every per-tick hash is bit-identical across the three legs — UI presence never moved sim state.
    CHECK(with_null.size() == absent.size() && with_gpu.size() == absent.size());
    bool null_identical = true;
    bool gpu_identical = true;
    for (std::size_t i = 0; i < absent.size(); ++i)
    {
        if (with_null[i] != absent[i])
            null_identical = false;
        if (with_gpu[i] != absent[i])
            gpu_identical = false;
    }
    CHECK(null_identical); // UI absent  == UI via null provider
    CHECK(gpu_identical);  // UI absent  == UI via GPU provider
    CHECK(absent.back() == with_null.back() && absent.back() == with_gpu.back());

    return report("m7-exit-4-determinism-presentation",
                  "hash_world bit-identical with UI absent / null-provider / GPU-provider (D6 pinned)");
}
