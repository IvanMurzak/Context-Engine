// Editor-panel accessibility enforcement harness (R-A11Y-001 / R-EDIT-001): the hardened successor
// to the F0b a11y-harness *hook*. It scans every registered editor panel over the headless
// context_gui_uitree model — no CEF, no browser — producing a machine-readable per-panel report
// (the semantic/ARIA audit + the keyboard-only focus order + the rendered DOM) that
// tools/a11y_scan.py gates in CI against the coverage manifest.
//
// Auditing the headless UI-logic tree (not the CEF-rendered live DOM) is deliberate: render_html()
// emits the panel's semantic HTML deterministically from this exact tree, so auditing the tree
// audits the DOM the CEF host paints — the same artifact on both sides of the CEF boundary (see
// gui/uitree/builtin.h). It therefore runs on the default 3-OS build matrix with zero GPU / zero CEF.

#pragma once

#include "context/editor/gui/uitree/panel.h"

#include <string>
#include <vector>

namespace context::editor::gui::a11y
{

// The accessibility scan result for ONE panel: the R-A11Y-001 semantic/ARIA audit findings, the
// keyboard-only focus order, the panel's exposed commands, and the rendered semantic HTML (so the CI
// gate can re-audit the real DOM string independently of the headless audit below).
struct PanelReport
{
    std::string id;
    std::string title;
    bool passed = false; // no violations (which, per audit_a11y, also implies every command has a
                         // keyboard path)
    std::vector<uitree::A11yViolation> violations;
    std::vector<std::string> focus_order; // node ids in keyboard (tab) order
    std::vector<std::string> commands;    // command ids the panel exposes
    std::string html;                     // rendered semantic HTML (uitree::render_html)
};

// Scan one panel: run the headless a11y audit + capture the keyboard focus order + render the DOM.
[[nodiscard]] PanelReport scan_panel(const std::string& id, const uitree::Panel& panel);

// Serialize a set of panel reports to a deterministic JSON document (stable key order, escaped
// strings) — the interchange format tools/a11y_scan.py consumes. Shape:
//   {"panels":[{"id","title","passed","violations":[{"node_id","code","message"}],
//               "focus_order":[...],"commands":[...],"html":"..."}]}
[[nodiscard]] std::string reports_to_json(const std::vector<PanelReport>& reports);

} // namespace context::editor::gui::a11y
