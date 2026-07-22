// The playbar's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 / R-EDIT-001),
// headless on the default matrix (no CEF). This is the M5-F5 half of the a11y coverage the M5-F6 harness
// reconciles (registered in a11y/registry.cpp + coverage.manifest.jsonl). Asserts the playbar has zero
// a11y violations AND all four transport commands (play/pause/stop/step) have a complete keyboard path,
// across every play state (edit / playing / paused) and after an error.
//
// M9 e08b: the states are reached the way they now really are — a daemon `play-state` fact and a
// gateway reply — never an in-process session the panel drove itself.

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

// A11y gate for one playbar state: zero violations AND every transport command reachable by keyboard
// (four focusable controls — play/pause/stop/step).
void assert_a11y_clean(const PlaybarModel& model)
{
    const uitree::Panel ui = build_playbar_panel(model);
    CHECK(uitree::audit_a11y(ui).empty());
    CHECK(uitree::focus_order(ui).size() == 4);
    CHECK(ui.has_command(kPlayCommand));
    CHECK(ui.has_command(kPauseCommand));
    CHECK(ui.has_command(kStopCommand));
    CHECK(ui.has_command(kStepCommand));
}

// A gateway that always refuses, so the "after an error" state is reached through the REAL refusal
// path (a daemon catalog code propagated verbatim) rather than a local state machine.
class RefusingGateway final : public PlayControlGateway
{
public:
    PlayCommandResult play() override { return refusal(); }
    PlayCommandResult pause() override { return refusal(); }
    PlayCommandResult stop() override { return refusal(); }
    PlayCommandResult step(std::uint64_t) override { return refusal(); }

private:
    static PlayCommandResult refusal()
    {
        PlayCommandResult out;
        out.ok = false;
        out.error_code = kPlayNotRunningCode;
        return out;
    }
};

} // namespace

int main()
{
    // edit (default) state.
    {
        PlaybarModel model;
        assert_a11y_clean(model);
    }

    // playing + paused, adopted from the daemon's `play-state` facts (the live path).
    {
        PlaybarModel model;
        CHECK(model.apply_play_state(PlayState::playing, 3));
        assert_a11y_clean(model); // playing
        CHECK(model.apply_play_state(PlayState::paused, 3));
        assert_a11y_clean(model); // paused
    }

    // after an error (a refused control leaves last_error set) the panel stays a11y-clean — the error
    // surfaces as status text, not as an unlabelled / unreachable node.
    {
        RefusingGateway gateway;
        PlaybarModel model(&gateway);
        const PlayAction action = model.step();
        CHECK(!action.ok);
        CHECK(model.last_error() == std::string(kPlayNotRunningCode));
        assert_a11y_clean(model);
    }

    PLAYBAR_TEST_MAIN_END();
}
