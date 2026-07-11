// Problems-panel view model (M5-F4, R-HUX-005 / R-FILE-003 / R-BRIDGE-008): the panel-facing,
// CEF-free projection of the R-FILE-003 structured diagnostics the Problems panel renders. Kept
// separate from the panel's uitree rendering so the grouping / severity / stability logic is asserted
// on its own and the panel renders a stable projection. Read-only: an observer view of the
// diagnostics/event stream, never a write path, and it MINTS NO new error-catalog codes.

#pragma once

#include "context/editor/bridge/event_stream.h" // bridge::Stability

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::gui::panels::problems
{

// Diagnostic severity (R-HUX-005 "grouping/severity"). The enum order is worst-first, so a stable sort
// by the underlying value groups the most severe diagnostics at the top of each file group.
enum class Severity
{
    error,
    warning,
    info,
    hint,
};

// The stable lowercase severity token shown in the row + used by the a11y-legible label.
[[nodiscard]] const char* severity_name(Severity s);

// A navigation target (R-FILE-003 JSON-pointer + line/column; R-HUX-005 click-to-navigate to the
// offending file/location). `file` empty => the diagnostic has no navigable source (e.g. a project-
// wide diagnostic). `line`/`column` are 1-based; 0 means "unspecified". `pointer` is a JSON-pointer
// within the file (R-FILE-003) — used when a diagnostic points into structured content rather than a
// text line.
struct NavTarget
{
    std::string file;
    std::string pointer;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    [[nodiscard]] bool navigable() const noexcept { return !file.empty(); }

    // The human-legible location for the row text: "file:line:col", "file:line", "file#pointer", or
    // "file" — whichever fields are present. Empty when not navigable.
    [[nodiscard]] std::string display() const;
};

// Project the R-BRIDGE-008 three-state stability onto the R-FILE-003 provisional/stable distinction
// R-HUX-005 draws: a diagnostic emitted while the derived world is still churning (`settling`) or that
// the next derivation may invalidate (`unstable`) is PROVISIONAL — visually distinguished from a
// `stable` one. Reuses the canonical bridge enum rather than minting a new stability field.
[[nodiscard]] bool is_provisional(bridge::Stability s) noexcept;

// One diagnostic the Problems panel renders — the panel-facing projection of an R-FILE-003 structured
// diagnostic carried on the bridge `diagnostics` topic. `code` is an EXISTING error-catalog code
// (R-CLI-008); this panel mints NO new codes. `generation` is the derived-world generation stamp
// (R-BRIDGE-008 / R-FILE-003) that lets the panel discard stale markers on the next settle.
struct ProblemDiagnostic
{
    std::string key; // optional caller-supplied stable identity for promotion/dedup across re-emission
    std::string code;
    std::string message;
    Severity severity = Severity::error;
    NavTarget nav;
    bridge::Stability stability = bridge::Stability::stable;
    std::uint64_t generation = 0;

    [[nodiscard]] bool provisional() const noexcept { return is_provisional(stability); }

    // The dedup / promotion identity: `key` when set, else a deterministic composite of the code and
    // navigation target and message. Two emissions of the SAME diagnostic (same identity) are one
    // problem — a re-emission with an advanced `stability` promotes it in place rather than duplicating.
    [[nodiscard]] std::string identity() const;
};

// An inline editor marker (R-HUX-005 "inline markers in the relevant editors"): the headless
// projection of the squiggle an editor draws at a diagnostic's location. Derived from the SAME
// diagnostics as the list, so the list and the markers never diverge. Only navigable diagnostics
// (those with a file) yield an inline marker.
struct InlineMarker
{
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    Severity severity = Severity::error;
    bool provisional = false;
    std::string code;
};

// A group of diagnostics sharing one file (R-HUX-005 grouping). `file` empty => the "(no file)" group
// of diagnostics with no navigable source. Within a group, diagnostics are worst-severity-first,
// stable among equal severities (first-seen order preserved).
struct ProblemGroup
{
    std::string file;
    std::vector<ProblemDiagnostic> diagnostics;
};

// The Problems view model: the current diagnostic set grouped by file plus the derived counts the
// status line surfaces. Deterministic — groups in first-seen file order, rows worst-severity-first.
struct ProblemsModel
{
    std::vector<ProblemGroup> groups;
    std::size_t total = 0;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t infos = 0;
    std::size_t hints = 0;
    std::size_t provisional = 0;

    [[nodiscard]] bool empty() const noexcept { return total == 0; }
};

// Build the grouped view model from a flat diagnostic list. Deterministic and total: groups appear in
// first-seen file order; within a group diagnostics are stable-sorted worst-severity-first; the
// total / per-severity / provisional counts are filled. Diagnostics with a duplicate identity() are
// collapsed (the LAST occurrence wins — the promotion path), so the model never renders one problem
// twice.
[[nodiscard]] ProblemsModel build_problems_model(const std::vector<ProblemDiagnostic>& diagnostics);

// The inline markers (R-HUX-005) for the diagnostic set — one per navigable diagnostic, in the model's
// grouped/row order, so the markers and the list are the same set drawn two ways.
[[nodiscard]] std::vector<InlineMarker>
build_inline_markers(const std::vector<ProblemDiagnostic>& diagnostics);

} // namespace context::editor::gui::panels::problems
