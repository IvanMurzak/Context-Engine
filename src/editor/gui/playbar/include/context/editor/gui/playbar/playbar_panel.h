// The play-in-editor PLAYBAR panel (M5-F5, issue #166; R-EDIT-001 / R-A11Y-001 / L-51 / R-HUX-011):
// projects the playbar model into a headless context_gui_uitree Panel — a LOUD play-mode indicator
// (L-51: the running session's runtime state is discarded on stop, never written to files), a status
// line carrying the play state + simTick + observed-frame summary + any reserved play.* error, and four
// keyboard-reachable transport controls (play/resume, pause, stop, step) each bound to a command so the
// whole play loop has a complete keyboard path. a11y-conformant by construction (uitree::audit_a11y
// returns no violations for any state) and deterministic (identical state -> byte-identical
// render_html). Built WITHOUT CEF, exactly like the sibling viewport / inspector panels.

#pragma once

#include "context/editor/gui/playbar/playbar_model.h"

#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::playbar
{

// Build the headless uitree Panel for the playbar's current state. Free function over a const model
// (the model is the stateful driver; rendering is pure over it — the same split as uitree::render_html
// / audit_a11y). The default-constructed model renders the a11y-clean edit-state panel the M5-F6 a11y
// harness scans.
[[nodiscard]] uitree::Panel build_playbar_panel(const PlaybarModel& model);

} // namespace context::editor::gui::playbar
