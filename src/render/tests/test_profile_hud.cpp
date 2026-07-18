// R-QA-013 test for the L-47 profiler HUD overlay (profile_hud.h): the enabled path builds the
// expected counter rows incl. a warn-tinted GC breach + the n/a path (happy), and — the DoD's
// "zero cost measured when disabled" claim — the disabled path produces NO rows, runs NO build, and
// does not read the model even when it is poisoned with NaN (edge/failure). GPU-free: the HUD emits
// rows a backend rasterizes, so this is a LOCAL gate on every toolchain.

#include "context/render/profile_hud.h"

#include "render_test.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using context::render::HudLine;
using context::render::HudTint;
using context::render::ProfileHud;
using context::render::ProfileHudModel;

namespace
{
bool any_contains(const std::vector<HudLine>& lines, const std::string& needle)
{
    for (const HudLine& l : lines)
        if (l.text.find(needle) != std::string::npos)
            return true;
    return false;
}

ProfileHudModel demo_model()
{
    ProfileHudModel m;
    m.frame_ms = 4.17;
    m.tick_hz = 60;
    m.native_ms = 2.10;
    m.script_ms = 1.50;
    m.worst_system = "motion";
    m.worst_ms = 0.90;
    m.gc_available = true;
    m.gc_pause_ms = 0.30;
    m.gc_within_budget = true;
    return m;
}
} // namespace

int main()
{
    // --- zero cost when disabled: no rows, no build, model never read -----------------------------
    {
        ProfileHud hud; // disabled by default
        CHECK(!hud.enabled());

        // A poisoned model: if the disabled path read ANY field, formatting NaN would still not
        // crash — but it MUST produce no rows and never bump the build counter.
        ProfileHudModel poison;
        poison.frame_ms = std::nan("");
        poison.native_ms = std::nan("");
        poison.gc_available = true;
        poison.gc_pause_ms = std::nan("");

        for (int i = 0; i < 1000; ++i)
        {
            const std::vector<HudLine>& rows = hud.update(poison);
            CHECK(rows.empty());
        }
        CHECK(hud.lines().empty());
        CHECK(hud.frames_built() == 0); // the build path never ran while disabled
    }

    // --- enabled: the counter rows are built ------------------------------------------------------
    {
        ProfileHud hud;
        hud.set_enabled(true);
        CHECK(hud.enabled());

        const std::vector<HudLine>& rows = hud.update(demo_model());
        CHECK(!rows.empty());
        CHECK(hud.frames_built() == 1);
        CHECK(any_contains(rows, "frame 4.17 ms"));
        CHECK(any_contains(rows, "60 Hz"));
        CHECK(any_contains(rows, "native 2.10 ms"));
        CHECK(any_contains(rows, "script 1.50 ms"));
        CHECK(any_contains(rows, "worst motion"));
        CHECK(any_contains(rows, "gc 0.30 ms"));
        CHECK(any_contains(rows, "(ok)"));

        // A second frame rebuilds (buffer reused) — the row count is stable, the counter advances.
        const std::size_t count_a = rows.size();
        const std::vector<HudLine>& rows2 = hud.update(demo_model());
        CHECK(hud.frames_built() == 2);
        CHECK(rows2.size() == count_a);
    }

    // --- enabled: a GC budget breach warn-tints the gc row ----------------------------------------
    {
        ProfileHud hud;
        hud.set_enabled(true);
        ProfileHudModel m = demo_model();
        m.gc_pause_ms = 9.9;
        m.gc_within_budget = false;
        const std::vector<HudLine>& rows = hud.update(m);
        bool found_warn = false;
        for (const HudLine& l : rows)
            if (l.tint == HudTint::Warn && l.text.find("OVER budget") != std::string::npos)
                found_warn = true;
        CHECK(found_warn);
    }

    // --- enabled: the no-VM (gc unavailable) path shows "gc n/a" -----------------------------------
    {
        ProfileHud hud;
        hud.set_enabled(true);
        ProfileHudModel m = demo_model();
        m.gc_available = false;
        const std::vector<HudLine>& rows = hud.update(m);
        CHECK(any_contains(rows, "gc n/a"));
    }

    // --- toggling off releases the rows (returns to the empty disabled state) ---------------------
    {
        ProfileHud hud;
        hud.set_enabled(true);
        hud.update(demo_model());
        CHECK(!hud.lines().empty());
        hud.set_enabled(false);
        CHECK(hud.lines().empty());
        const std::vector<HudLine>& rows = hud.update(demo_model());
        CHECK(rows.empty()); // disabled again -> no work
    }

    RENDER_TEST_MAIN_END();
}
