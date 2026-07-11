// The editor-panel registry the a11y harness scans (R-A11Y-001). Maps a panel id to a headless
// factory so the harness can instantiate + audit every built-in panel WITHOUT booting CEF. This is
// the C++ half of the per-panel coverage contract; its DATA half is coverage.manifest.jsonl. A panel
// is only "covered" when it appears in BOTH — tools/a11y_scan.py cross-checks the harness report
// (driven by this registry) against the manifest, so adding a panel to one without the other fails CI.
//
// EXTENSION POINT (M5 fan-out): a new built-in panel (scene tree / inspector / Problems / play bar /
// viewport) appends ONE RegisteredPanel entry to registered_panels() in registry.cpp AND ONE line to
// coverage.manifest.jsonl. Both are append-only shared anchors (union-merge — see .gitattributes) so
// concurrent M5 waves stay conflict-free.

#pragma once

#include "context/editor/gui/uitree/panel.h"

#include <functional>
#include <string>
#include <vector>

namespace context::editor::gui::a11y
{

// A headless factory for a built-in panel — instantiates the exact UI-logic tree the CEF host would
// render, so the a11y audit runs without CEF.
using PanelFactory = std::function<uitree::Panel()>;

struct RegisteredPanel
{
    std::string id;
    PanelFactory factory;
};

// Every built-in editor panel that must pass the R-A11Y-001 gate, in a stable order. The harness
// scans each; the CI gate cross-checks this set against coverage.manifest.jsonl.
[[nodiscard]] std::vector<RegisteredPanel> registered_panels();

} // namespace context::editor::gui::a11y
