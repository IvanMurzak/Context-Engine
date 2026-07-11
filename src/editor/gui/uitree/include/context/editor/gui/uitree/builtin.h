// Built-in placeholder panel (M5-F0b): the one panel the CEF editor host renders in its boot smoke,
// defined HEADLESS here so the exact tree the host paints is also the tree the default-matrix a11y
// ctest audits — the same artifact on both sides of the CEF boundary. A later M5 task replaces it
// with the real scene-tree / inspector / Problems panels, each built on this same UI-logic tree.

#pragma once

#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::uitree
{

// A minimal, a11y-conformant placeholder panel: a labelled region with a heading, a status line, and
// one focusable, command-bound button ("refresh"). Conformant by construction — audit_a11y() returns
// no violations — so it doubles as the positive fixture for the a11y-harness hook.
[[nodiscard]] Panel make_placeholder_panel();

} // namespace context::editor::gui::uitree
