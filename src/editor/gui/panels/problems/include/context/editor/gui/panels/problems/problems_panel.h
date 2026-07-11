// The Problems observer panel (M5-F4, R-HUX-005 / R-FILE-003 / R-BRIDGE-008 / R-A11Y-001 / R-HUX-011):
// projects the R-FILE-003 structured diagnostics into a headless context_gui_uitree Panel — a list
// grouped by file plus the inline markers editors draw — drives click-to-navigate (emitting a
// navigation event the relevant editors consume), and handles the provisional->stable promotion +
// stale-marker discard on the R-BRIDGE-008 derivation.settled event. Read-only observer: no writes
// into the world, no new error-catalog codes. The whole panel is CI-assertable WITHOUT booting CEF.

#pragma once

#include "context/editor/gui/panels/problems/problems_model.h"

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h" // bridge::Stability

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::editor::gui::panels::problems
{

// The navigation event other editors consume to jump to a diagnostic's source (R-HUX-005
// click-to-navigate; the R-HUX-011 selection/navigation loop). An empty `target.file` means the
// navigation was cleared. `diagnostic_identity` is the identity of the navigated diagnostic (empty
// when cleared) so a consumer can correlate the jump with the list row.
struct ProblemNavigation
{
    std::string diagnostic_identity;
    NavTarget target;
};

// The command a focusable diagnostic row binds so click-to-navigate has a keyboard path (R-CLI-001 /
// R-A11Y-001 — every GUI action reachable without a pointer).
inline constexpr const char* kNavigateCommand = "problems.navigate";

class ProblemsPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.problems";

    ProblemsPanel() = default;

    // Replace the diagnostic set from a fresh bridge snapshot. Diagnostics with a duplicate identity
    // are collapsed (last wins). Preserves the current navigation when its diagnostic identity still
    // exists in the new set; clears it (notifying listeners) otherwise.
    void set_diagnostics(std::vector<ProblemDiagnostic> diagnostics);

    // Ingest/merge one diagnostic (a `diagnostics` topic delta). Replaces an existing diagnostic with
    // the same identity — the provisional->stable PROMOTION path: a re-emitted diagnostic whose
    // stability advanced updates IN PLACE, never duplicating — or appends a new one. Returns true iff
    // it updated an existing entry.
    bool ingest(ProblemDiagnostic diagnostic);

    [[nodiscard]] const std::vector<ProblemDiagnostic>& diagnostics() const noexcept
    {
        return diagnostics_;
    }

    // The grouped, severity-ordered view model (built on demand from the current diagnostic set).
    [[nodiscard]] ProblemsModel model() const { return build_problems_model(diagnostics_); }

    // The inline markers editors draw (R-HUX-005), derived from the same diagnostic set as the list.
    [[nodiscard]] std::vector<InlineMarker> inline_markers() const
    {
        return build_inline_markers(diagnostics_);
    }

    // R-BRIDGE-008 / R-FILE-003: consume a derivation.settled event. In order: (1) DISCARD stale
    // provisional diagnostics stamped with a generation OLDER than the settled one (a settling pass
    // superseded them); (2) PROMOTE provisional diagnostics stamped with the settled generation to
    // `stable`. Records the settled generation + the world's reported stability for the status line. A
    // navigation whose diagnostic was discarded is cleared (notifying).
    void on_derivation_settled(std::uint64_t generation, bridge::Stability stability);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bridge::Stability stability() const noexcept { return stability_; }

    // Click-to-navigate. `navigate` sets the current navigation to the diagnostic `identity` (which
    // must exist AND be navigable — an unknown or non-navigable identity is ignored and returns
    // false), notifies every registered listener, and returns true. `clear_navigation` resets to empty
    // and notifies.
    bool navigate(const std::string& identity);
    void clear_navigation();
    [[nodiscard]] const ProblemNavigation& navigation() const noexcept { return navigation_; }

    // Register a listener the relevant editors use to react to click-to-navigate (R-HUX-005 / R-HUX-011).
    using NavigationListener = std::function<void(const ProblemNavigation&)>;
    void add_navigation_listener(NavigationListener listener);

    // Build the headless uitree Panel for the current diagnostic set + navigation + generation/
    // stability. Deterministic: identical state produces a byte-identical Panel (uitree::render_html),
    // so a re-render on settle with an unchanged world is stable. a11y-conformant by construction —
    // uitree::audit_a11y returns no violations for any diagnostic set.
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    void notify() const;
    [[nodiscard]] const ProblemDiagnostic* find(const std::string& identity) const;

    std::vector<ProblemDiagnostic> diagnostics_;
    ProblemNavigation navigation_;
    std::uint64_t generation_ = 0;
    bridge::Stability stability_ = bridge::Stability::stable;
    std::vector<NavigationListener> listeners_;
};

} // namespace context::editor::gui::panels::problems
