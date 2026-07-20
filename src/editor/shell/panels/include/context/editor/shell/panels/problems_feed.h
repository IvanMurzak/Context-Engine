// The LIVE diagnostics feed for the Problems panel (M9 e05d1, design 04 §4) — the projection from
// the daemon's `diagnostics` event topic onto the headless `ProblemsPanel` model, plus the
// `PanelProvider` that publishes it through the Shell's panel host.
//
// WHERE THIS SITS. The Shell is an ordinary client (D10): it subscribes to the daemon's event stream
// through the published `context_client` SDK (`SubscriptionConsumer`), and this file turns what
// arrives there into the panel model's own vocabulary. It is the READ path of design 04 §4 step 3
// ("subscribes to the panel's change events"), and it is deliberately the ONLY place the wire shape
// of a diagnostic is interpreted.
//
// WHY THE PARSERS ARE FREE FUNCTIONS. Everything below `parse_*` is pure: JSON in, model out, no
// daemon, no socket, no clock. That is what makes the live path testable at T1 on all three default
// `build` legs — a feed whose only test needs a running daemon is a feed nobody tests. The daemon
// wiring itself (a `SubscriptionConsumer` pumped NON-BLOCKINGLY from the owner loop —
// `poll_timeout_ms = 0`, single-threaded by design because this projection mutates the very models
// the bridge handlers render on that same thread) lives in `src/editor/shell/app/editor_main.cpp`,
// where it can be replaced without touching this projection.
//
// TOLERANCE IS A DESIGN CHOICE, NOT LAZINESS. The `diagnostics` topic carries payloads from several
// publishers (crash-recovery diagnostics from `EditorKernel::start`, derivation diagnostics, future
// ones), and their shapes are NOT identical: some carry a flat `file`/`line`, some a nested
// location, some neither. A parser that demanded one exact shape would silently drop everything
// else — a Problems panel that is quietly empty is strictly worse than one that shows a diagnostic
// with less navigation detail than it could have. So every field but `code`/`message` is optional
// with a documented default, and a payload carrying NEITHER is the only thing rejected.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/problems/problems_model.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell::panels
{

namespace problems = gui::panels::problems;

// The event topics this feed consumes. Both are published names on the daemon's stream (the
// generated client schema lists them), mirrored here so the subscription and the dispatch below
// cannot disagree about which strings they mean.
inline constexpr const char* kDiagnosticsTopic = "diagnostics";
inline constexpr const char* kDerivationTopic = "derivation";

// The `derivation` event that settles the panel's provisional markers (R-BRIDGE-008).
inline constexpr const char* kDerivationSettledEvent = "derivation.settled";

// --------------------------------------------------------------------------------- pure parsers

// Map a wire severity token onto the panel's severity. UNKNOWN TOKENS BECOME `error`, deliberately:
// a diagnostic whose severity we cannot read is more likely to matter than not, and silently
// downgrading it to `hint` would bury it at the bottom of the list.
[[nodiscard]] problems::Severity parse_severity(const std::string& token);

// Map a wire stability token (bridge::stability_name's vocabulary). Anything unrecognized — and an
// absent field — is `stable`, matching the event stream's own default for the topic.
[[nodiscard]] bridge::Stability parse_stability(const std::string& token);

// Project one `diagnostics` payload. `generation` comes from the EVENT ENVELOPE, not the payload:
// the stream stamps every event with the derived-world generation, and that is the stamp the panel
// discards stale provisional markers against on the next settle.
//
// nullopt when the payload is not an object, or carries neither a `code` nor a `message` — the one
// shape that cannot be rendered as anything a human could act on.
[[nodiscard]] std::optional<problems::ProblemDiagnostic>
parse_diagnostic(const contract::Json& payload, std::uint64_t generation);

// Project a subscription SNAPSHOT into the full diagnostic set. Accepts the three shapes a snapshot
// legitimately takes — a bare array of diagnostics, an object with a `diagnostics` array, or an
// object with an `events` array of full envelopes — because the snapshot shape is the daemon's to
// choose and hardcoding one would make the panel silently empty if it chose another. Anything
// unparseable within an accepted container is SKIPPED, never fatal.
//
// nullopt when the snapshot carries NO recognized container — which is a different answer from an
// engaged-but-empty vector. Today's `SubscriptionConsumer` snapshot is a CURSOR
// (`{incarnationId, generation, lastSeq}`) with no diagnostic container at all, so conflating the
// two would have the caller CLEAR the panel on every resnapshot. Engaged-and-empty means "the set
// is genuinely empty" and does clear; nullopt means "this snapshot said nothing about the set".
[[nodiscard]] std::optional<std::vector<problems::ProblemDiagnostic>>
parse_diagnostics_snapshot(const contract::Json& snapshot, std::uint64_t generation);

// ------------------------------------------------------------- the node-id -> diagnostic mapping

// The prefix `ProblemsPanel::build_panel` gives every diagnostic row (`problems.row.<n>`, indexed in
// the model's grouped/row order). Named here rather than spelled inline so the one place that
// depends on it is greppable from both sides.
inline constexpr const char* kProblemsRowPrefix = "problems.row.";

// Resolve an activated NODE id to the diagnostic identity `ProblemsPanel::navigate` expects.
//
// WHY THIS MAPPING EXISTS AT ALL. The hydration runtime dispatches the id of the node the user
// activated — it cannot send a diagnostic identity, because it has no idea what a diagnostic is, and
// teaching it would be exactly the panel-specific special-casing e05d3 must not inherit. So the
// translation lives HERE, in the Problems provider, which is the layer entitled to know both
// vocabularies. The host and the runtime stay agnostic; this function absorbs the difference.
//
// It re-derives the row order from the panel's own model, which is the SAME traversal `build_panel`
// uses, so the two cannot disagree about which row index means which diagnostic. nullopt for a node
// that is not a row, an out-of-range index, or a non-navigable diagnostic.
[[nodiscard]] std::optional<std::string> problems_row_identity(const problems::ProblemsPanel& panel,
                                                               const std::string& node_id);

// ------------------------------------------------------------------------------------ the feed

// Owns a ProblemsPanel and drives it from the live stream, touching the PanelHost on every change so
// the next `panel.render` is seen as fresh by the hydration runtime.
class ProblemsFeed
{
public:
    // Non-owning: `host` must outlive the feed. `panel_id` is passed rather than hardcoded — the
    // feed is a MECHANISM the composition root points at a roster id, not a second place that knows
    // which panel Problems is.
    ProblemsFeed(PanelHost& host, std::string panel_id);

    ProblemsFeed(const ProblemsFeed&) = delete;
    ProblemsFeed& operator=(const ProblemsFeed&) = delete;
    ProblemsFeed(ProblemsFeed&&) = delete;
    ProblemsFeed& operator=(ProblemsFeed&&) = delete;

    // Replace the diagnostic set from a fresh snapshot (subscribe, gap recovery, incarnation change).
    void apply_snapshot(const contract::Json& snapshot, std::uint64_t generation);

    // Apply one delivered event. Returns true when the panel's model actually changed — the caller
    // may use that to avoid touching the host on an irrelevant topic. Unknown topics are ignored.
    bool apply_event(const std::string& topic, const contract::Json& payload,
                     std::uint64_t generation);

    [[nodiscard]] problems::ProblemsPanel& panel() noexcept { return panel_; }
    [[nodiscard]] const problems::ProblemsPanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t snapshots_applied() const noexcept { return snapshots_applied_; }
    [[nodiscard]] std::size_t events_applied() const noexcept { return events_applied_; }

    // The provider to bind on the PanelHost. It captures `this`, so the feed must OUTLIVE the host
    // binding — at the composition root both live for the process's lifetime.
    //
    // Problems is a READ-ONLY OBSERVER (R-HUX-005), so the provider deliberately supplies no
    // `gesture` and no state pair: the panel has no continuous gesture and persists nothing. Those
    // absences are REPORTED by `panel.list` rather than stubbed, which is what keeps the runtime
    // from ever sending a verb this panel could only refuse.
    [[nodiscard]] PanelProvider make_provider();

private:
    PanelHost& host_;
    std::string panel_id_;
    problems::ProblemsPanel panel_;
    std::size_t snapshots_applied_ = 0;
    std::size_t events_applied_ = 0;
};

} // namespace context::editor::shell::panels
