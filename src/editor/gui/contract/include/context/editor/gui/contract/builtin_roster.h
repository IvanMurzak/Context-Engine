// The BUILT-IN editor-UI roster (M9 e05b, design 04 §3): the ONE authoritative list of the panels the
// editor ships, as panel-manifest-v2 Contributions.
//
// Before M9 this list existed twice — once as a stack-local ExtensionRegistry inside the CEF host
// (which registered a single placeholder) and once as the hand-maintained a11y scan list
// (gui/a11y/registry.cpp) — and the two could silently disagree, which is exactly how the Problems
// panel once shipped a11y-uncovered (#168). The roster below is now the single source of truth:
//
//   * the editor host builds its ExtensionRegistry from it (make_builtin_registry) — the single
//     GLOBAL roster, deny-by-default preserved;
//   * gui/a11y/registry.cpp DERIVES registered_panels() from it, binding each roster id to a headless
//     factory, and the standing gui-a11y-coverage ctest asserts roster == factories == scanned ==
//     coverage.manifest.jsonl in BOTH directions. A hand-edit to any one anchor therefore cannot
//     drift the others — it fails that ctest on the default 3-OS build matrix.
//
// Deliberately DATA-ONLY (ids/titles/manifests, no panel headers, no factories): it lives in the
// low-level contract library so the CEF host can consume it without linking every panel library into
// the CEF-ON build. The id strings are cross-checked against each panel class's own kContributionId by
// the a11y coverage ctest, so a rename cannot drift them either.

#pragma once

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"

#include <vector>

namespace context::editor::gui::contract
{

// Every built-in editor-UI contribution, in the stable roster order the a11y scan and the panel
// listing follow. Built once on first call.
[[nodiscard]] const std::vector<Contribution>& builtin_contributions();

// The single global roster: an ExtensionRegistry carrying every builtin_contributions() entry.
// Deny-by-default is fully preserved — each entry goes through register_contribution(), so a built-in
// that violated a contract invariant would be REFUSED exactly like a third-party one. `all_ok` (when
// non-null) reports whether every built-in registered; a false value is a build-time defect in the
// roster above, asserted by the gui-contract-test_roster ctest.
[[nodiscard]] ExtensionRegistry make_builtin_registry(bool* all_ok = nullptr);

} // namespace context::editor::gui::contract
