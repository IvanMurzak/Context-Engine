// Playbar panel: the play-in-editor transport projected into a headless uitree Panel with the L-51
// loud play-mode indicator + the four keyboard-reachable transport controls.

#include "context/editor/gui/playbar/playbar_panel.h"

#include "context/editor/gui/uitree/node.h"

#include <sstream>
#include <string>

namespace context::editor::gui::playbar
{

namespace
{

// The LOUD play-mode indicator text (L-51: the running session's runtime state is discarded on stop,
// never written to files — surfaced prominently so live-edit-during-play can never be mistaken for
// authoring). Deterministic.
[[nodiscard]] std::string mode_text(PlayState state)
{
    switch (state)
    {
    case PlayState::edit:
        return "EDIT MODE - authored state (file-authoritative)";
    case PlayState::playing:
        return "PLAY MODE - running; runtime state is discarded on stop (L-51)";
    case PlayState::paused:
        return "PLAY MODE (paused) - runtime state is discarded on stop (L-51)";
    }
    return "EDIT MODE - authored state (file-authoritative)";
}

// A human/AT-facing summary of the observed play frame's drawables + lights (the same shape the
// viewport uses), or "no live scene" in edit state.
[[nodiscard]] std::string scene_text(const PlayFrame& frame, PlayState state)
{
    if (state == PlayState::edit)
    {
        return "no live scene";
    }
    const std::size_t drawables = frame.snapshot.items.size();
    const std::size_t lights =
        frame.snapshot.directional_lights.size() + frame.snapshot.point_lights.size();
    std::ostringstream out;
    out << drawables << (drawables == 1 ? " drawable" : " drawables");
    if (lights > 0)
    {
        out << ", " << lights << (lights == 1 ? " light" : " lights");
    }
    return out.str();
}

// The status line: the play state + simTick + observed-frame summary + the outcome (ready, or the last
// reserved play.* error). Deterministic.
[[nodiscard]] std::string status_text(const PlaybarModel& model)
{
    std::ostringstream out;
    out << "Play bar - " << state_token(model.state()) << " - tick " << model.sim_tick() << " - "
        << scene_text(model.last_frame(), model.state()) << " - "
        << (model.last_error().empty() ? "ready" : model.last_error());
    return out.str();
}

// The play/resume control's accessible label depends on the state (resume when paused). Its node id +
// bound command stay stable so the a11y tree is invariant across states.
[[nodiscard]] const char* play_label(PlayState state)
{
    return state == PlayState::paused ? "Resume" : "Play";
}

} // namespace

uitree::Panel build_playbar_panel(const PlaybarModel& model)
{
    using uitree::Role;
    using uitree::UiNode;

    uitree::Panel panel("playbar", "Play Bar");
    // Every transport control has a keyboard/CLI path (R-A11Y-001 / R-CLI-001). Each command below is
    // bound to a focusable button, so none is ever an unreachable a11y orphan.
    panel.add_command(kPlayCommand, "Play");
    panel.add_command(kPauseCommand, "Pause");
    panel.add_command(kStopCommand, "Stop");
    panel.add_command(kStepCommand, "Step one tick");

    UiNode root(Role::region, "playbar.panel");
    root.set_label("Play Bar");

    root.add_child(
        UiNode(Role::heading, "playbar.heading").set_label("Play Bar").set_text("Play Bar"));

    // The L-51 loud play-mode indicator — a live status region prominently naming the mode.
    root.add_child(UiNode(Role::status, "playbar.mode")
                       .set_label("Play mode")
                       .set_text(mode_text(model.state())));

    // The transport status line (state + simTick + observed frame + outcome).
    root.add_child(UiNode(Role::status, "playbar.status")
                       .set_label("Play status")
                       .set_text(status_text(model)));

    // The four transport controls, all focusable + command-bound (complete keyboard path). Present in
    // every state (the a11y tree is invariant); the CEF host greys out an inapplicable control, but no
    // state removes a command from the keyboard-reachable set.
    root.add_child(UiNode(Role::button, "playbar.play")
                       .set_label(play_label(model.state()))
                       .set_text(play_label(model.state()))
                       .set_focusable(true)
                       .set_command(kPlayCommand));
    root.add_child(UiNode(Role::button, "playbar.pause")
                       .set_label("Pause")
                       .set_text("Pause")
                       .set_focusable(true)
                       .set_command(kPauseCommand));
    root.add_child(UiNode(Role::button, "playbar.stop")
                       .set_label("Stop")
                       .set_text("Stop")
                       .set_focusable(true)
                       .set_command(kStopCommand));
    root.add_child(UiNode(Role::button, "playbar.step")
                       .set_label("Step one tick")
                       .set_text("Step")
                       .set_focusable(true)
                       .set_command(kStepCommand));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::playbar
