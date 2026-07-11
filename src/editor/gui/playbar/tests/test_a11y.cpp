// The playbar's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 / R-EDIT-001),
// headless on the default matrix (no CEF). This is the M5-F5 half of the a11y coverage the M5-F6 harness
// reconciles (registered in a11y/registry.cpp + coverage.manifest.jsonl; the defensive fragment is
// a11y/coverage/playbar.json). Asserts the playbar has zero a11y violations AND all four transport
// commands (play/pause/stop/step) have a complete keyboard path, across every play state (edit / playing
// / paused), after an error, and with a populated observed frame.

#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/playbar/session_control.h"

#include "context/editor/gui/uitree/panel.h"

#include "playbar_test.h"

#include <cstdint>

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

class FrameStub : public SessionControl
{
public:
    std::uint32_t drawables = 0;
    ControlOutcome start() override { return f(); }
    ControlOutcome step(std::uint64_t) override { return f(); }
    void discard() override {}
    HotReloadOutcome apply_hot_reload(const LiveEdit&) override
    {
        HotReloadOutcome o;
        o.ok = true;
        return o;
    }

private:
    ControlOutcome f() const
    {
        ControlOutcome o;
        o.ok = true;
        o.frame.snapshot.items.resize(drawables);
        return o;
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

    // playing (with a populated observed frame) + paused.
    {
        FrameStub control;
        control.drawables = 8;
        PlaybarModel model(&control);
        model.play();
        model.step(3);
        assert_a11y_clean(model); // playing
        model.pause();
        assert_a11y_clean(model); // paused
    }

    // after an error (a control issued in edit state leaves last_error set) the panel stays a11y-clean
    // (the error surfaces as status text, not as an unlabelled / unreachable node).
    {
        PlaybarModel model;
        (void)model.step(); // play.not_running -> last_error set
        assert_a11y_clean(model);
    }

    PLAYBAR_TEST_MAIN_END();
}
