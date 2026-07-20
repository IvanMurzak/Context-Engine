// The editor-panel registry the a11y harness scans (R-A11Y-001). Maps a panel id to a headless
// factory so the harness can instantiate + audit every built-in panel WITHOUT booting CEF.
//
// M9 e05b — THIS LIST IS NO LONGER HAND-MAINTAINED. registered_panels() is now REGENERATED from the
// single built-in roster (gui/contract/builtin_roster.h): it walks contract::builtin_contributions()
// in roster order and binds each id to its headless factory. The registry therefore cannot list a
// panel the roster does not declare, and the standing gui-a11y-coverage ctest additionally asserts the
// reverse — that every roster id HAS a factory, and that the roster, the factory table, and
// coverage.manifest.jsonl name exactly the same set. A hand-edit to any one anchor fails that ctest on
// the default 3-OS build matrix instead of silently shipping an uncovered panel (the #168 failure
// mode, where Problems landed before the harness and its coverage held only vacuously).
//
// EXTENSION POINT: a new built-in panel is a THREE-anchor edit — its Contribution in
// gui/contract/src/builtin_roster.cpp, its factory in registry.cpp (plus its library in this
// directory's CMakeLists.txt), and its line in coverage.manifest.jsonl. Miss one and the ctest names
// which.

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

// Every built-in editor panel that must pass the R-A11Y-001 gate, in ROSTER order (the order of
// contract::builtin_contributions()). The harness scans each; the CI gate cross-checks this set
// against coverage.manifest.jsonl. Derived, not hand-written — see the file header.
[[nodiscard]] std::vector<RegisteredPanel> registered_panels();

// The ids this translation unit can actually build a headless panel for — the FACTORY half of the
// derivation. Exposed so the standing gui-a11y-coverage ctest can assert the roster and the factory
// table agree in BOTH directions: a roster entry with no factory would be silently unscanned (it
// would simply not appear in registered_panels()), and a factory with no roster entry is dead code.
[[nodiscard]] std::vector<std::string> panel_factory_ids();

} // namespace context::editor::gui::a11y
