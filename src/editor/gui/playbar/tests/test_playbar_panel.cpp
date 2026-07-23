// The playbar panel rendering (R-QA-013): the L-51 loud play-mode indicator fed from DAEMON state
// (M9 e08b), the deterministic render_html, and the four keyboard-reachable transport controls.
//
// Every state below is reached through the two doors that exist since e08b — a `play-state` fact
// published by the daemon, and a gateway reply — never an in-process session the panel drove itself.
// The cross-panel "play output flows through the F1 viewport with no second render path" proof moved
// with the PlayFrame it is about, to test_session_control.cpp.

#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"

#include "context/editor/gui/uitree/panel.h"

#include "playbar_test.h"

#include <cstdint>
#include <string>

using namespace context::editor::gui::playbar;
namespace uitree = context::editor::gui::uitree;

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

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
        CHECK(contains(html, "EDIT MODE"));        // the loud L-51 indicator
        CHECK(contains(html, "no live session"));
        CHECK(contains(html, "role=\"button\""));
    }

    // --- playing panel: the indicator is fed by the DAEMON's fact, with no local write -------------
    {
        PlaybarModel model;
        CHECK(model.apply_play_state(PlayState::playing, 7));

        const uitree::Panel panel = build_playbar_panel(model);
        CHECK(uitree::audit_a11y(panel).empty());
        const std::string html = uitree::render_html(panel);
        CHECK(contains(html, "PLAY MODE"));         // loud L-51 indicator
        CHECK(contains(html, "discarded on stop")); // L-51 semantics surfaced
        CHECK(contains(html, "tick 7"));
        CHECK(contains(html, "live daemon session"));

        // Deterministic: identical state -> byte-identical render_html.
        const uitree::Panel again = build_playbar_panel(model);
        CHECK(uitree::render_html(again) == html);
    }

    // --- paused panel: the play control's label becomes "Resume" -----------------------------------
    {
        PlaybarModel model;
        CHECK(model.apply_play_state(PlayState::paused, 2));
        const std::string html = uitree::render_html(build_playbar_panel(model));
        CHECK(contains(html, "PLAY MODE (paused)"));
        CHECK(contains(html, "aria-label=\"Resume\""));
    }

    // --- the L-51 indicator vocabulary IS the daemon's token vocabulary ----------------------------
    // e08a publishes `state` as these exact strings (docs/editor-session-state.md), which is why the
    // indicator needs no translation layer that could drift. Pinned here, panel-side, so a rename of
    // either half fails locally rather than only in the cross-process T2 drill.
    {
        CHECK(std::string(state_token(PlayState::edit)) == "edit");
        CHECK(std::string(state_token(PlayState::playing)) == "playing");
        CHECK(std::string(state_token(PlayState::paused)) == "paused");
    }

    PLAYBAR_TEST_MAIN_END();
}
