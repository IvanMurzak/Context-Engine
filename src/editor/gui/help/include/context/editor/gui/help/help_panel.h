// The in-editor Help / getting-started panel (R-HUX-010): a headless context_gui_uitree Panel that
// surfaces the getting-started sample references (R-QA-006 human-onboarding set) and per-panel
// contextual help GENERATED from the live contract (help_model.h). The getting-started panel
// R-HUX-010 names. Read-only, offline, CEF-free — a11y-conformant by construction (audit_a11y returns
// no violations), so the default-matrix a11y ctest audits the exact tree the CEF host would paint.

#pragma once

#include "context/editor/gui/uitree/panel.h"

namespace context::editor::gui::help
{

class HelpPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under (a11y registry + coverage
    // manifest carry the matching `builtin.help` entry).
    static constexpr const char* kContributionId = "builtin.help";

    HelpPanel() = default;

    // Build the headless uitree Panel: a labelled region with a getting-started sample list and a
    // per-panel help list, every entry keyboard-reachable. Deterministic — identical registry +
    // corpus state produce a byte-identical Panel (uitree::render_html). a11y-conformant for the
    // shipped registry: uitree::audit_a11y returns no violations.
    [[nodiscard]] uitree::Panel build_panel() const;
};

} // namespace context::editor::gui::help
