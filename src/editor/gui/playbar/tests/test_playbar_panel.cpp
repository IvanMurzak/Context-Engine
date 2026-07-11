// The playbar panel rendering (R-QA-013) + the cross-panel proof that play output flows through the F1
// viewport with NO second render path: the PlayFrame the playbar produces IS render::RenderSnapshot,
// and feeding it to viewport::ViewportPanel::set_snapshot reproduces the same scene the observer viewport
// renders. Also asserts the L-51 loud play-mode indicator, the deterministic render_html, and the four
// keyboard-reachable transport controls.

#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/playbar/session_control.h"

#include "context/editor/gui/uitree/panel.h"
#include "context/editor/gui/viewport/viewport_panel.h"

#include "playbar_test.h"

#include <cstdint>
#include <string>

using namespace context::editor::gui::playbar;
namespace uitree = context::editor::gui::uitree;
namespace viewport = context::editor::gui::viewport;

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A minimal SessionControl double producing a frame with a fixed drawable count (enough to drive the
// panel + the viewport cross-check).
class FrameStub : public SessionControl
{
public:
    std::uint32_t drawables = 0;
    std::uint64_t tick = 0;

    ControlOutcome start() override
    {
        tick = 0;
        return ok_frame();
    }
    ControlOutcome step(std::uint64_t ticks) override
    {
        tick += ticks;
        return ok_frame();
    }
    void discard() override { tick = 0; }
    HotReloadOutcome apply_hot_reload(const LiveEdit&) override
    {
        HotReloadOutcome out;
        out.ok = true;
        out.frame = make_frame();
        return out;
    }

private:
    [[nodiscard]] PlayFrame make_frame() const
    {
        PlayFrame f;
        f.sim_tick = tick;
        f.snapshot.sim_tick = tick;
        f.snapshot.items.resize(drawables);
        return f;
    }
    [[nodiscard]] ControlOutcome ok_frame()
    {
        ControlOutcome out;
        out.ok = true;
        out.frame = make_frame();
        return out;
    }
};

} // namespace

int main()
{
    // --- default (edit) panel: identity, commands, loud EDIT-mode indicator, a11y-clean -----------
    {
        PlaybarModel model;
        const uitree::Panel panel = build_playbar_panel(model);
        CHECK(panel.id() == "playbar");
        CHECK(panel.title() == "Play Bar");
        CHECK(panel.has_command(kPlayCommand));
        CHECK(panel.has_command(kPauseCommand));
        CHECK(panel.has_command(kStopCommand));
        CHECK(panel.has_command(kStepCommand));

        // a11y-clean, and all four transport controls reachable by keyboard.
        CHECK(uitree::audit_a11y(panel).empty());
        CHECK(uitree::focus_order(panel).size() == 4);

        const std::string html = uitree::render_html(panel);
        CHECK(contains(html, "EDIT MODE"));      // the loud L-51 indicator
        CHECK(contains(html, "no live scene"));
        CHECK(contains(html, "role=\"button\""));
    }

    // --- playing panel: loud PLAY-mode indicator + observed frame summary + Play/Pause/Stop/Step ----
    {
        FrameStub control;
        control.drawables = 4;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        model.step(7);

        const uitree::Panel panel = build_playbar_panel(model);
        CHECK(uitree::audit_a11y(panel).empty());
        const std::string html = uitree::render_html(panel);
        CHECK(contains(html, "PLAY MODE"));                 // loud L-51 indicator
        CHECK(contains(html, "discarded on stop"));         // L-51 semantics surfaced
        CHECK(contains(html, "tick 7"));
        CHECK(contains(html, "4 drawables"));

        // Deterministic: identical state -> byte-identical render_html.
        const uitree::Panel again = build_playbar_panel(model);
        CHECK(uitree::render_html(again) == html);
    }

    // --- paused panel: the play control's label becomes "Resume" -----------------------------------
    {
        FrameStub control;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        CHECK(model.pause().ok);
        const std::string html = uitree::render_html(build_playbar_panel(model));
        CHECK(contains(html, "PLAY MODE (paused)"));
        CHECK(contains(html, "aria-label=\"Resume\""));
    }

    // --- the play output flows through the F1 viewport (NO second render path) ---------------------
    // The PlayFrame the playbar produces is render::RenderSnapshot; handing it to the observer viewport
    // reproduces the same scene the viewport renders (ViewportPanel::set_snapshot).
    {
        FrameStub control;
        control.drawables = 6;
        PlaybarModel model(&control);
        CHECK(model.play().ok);
        model.step(1);

        viewport::ViewportPanel vp;
        vp.set_snapshot(model.last_frame().snapshot); // the SAME snapshot type — no second render path
        CHECK(vp.scene().drawables == 6);

        // and the viewport observing the play frame is itself still a11y-clean.
        CHECK(uitree::audit_a11y(vp.build_panel()).empty());
    }

    PLAYBAR_TEST_MAIN_END();
}
