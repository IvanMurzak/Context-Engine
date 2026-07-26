// The LIVE inspector feed for the Inspector panel (M9 e05d3, design 04 §4 / 05 §7) — the projection
// from the daemon's `editor.inspect` composed read onto the headless `InspectorPanel` model, plus
// the `PanelProvider` that publishes it through the Shell's panel host.
//
// WHERE THIS SITS. The Shell is an ordinary client (D10/D18): the schema-driven field derivation
// runs on the daemon (gui/panels/builders/, served by `editor.inspect`), and what arrives here is
// the boundary-clean panel model AS DATA — plus the root scene's raw-byte CAS token, adopted as the
// panel's base hash so the wire write (e09b-2's WireOverrideWriteGateway) guards on live state.
// This file is deliberately the ONLY place that wire shape is interpreted; the parsers mirror
// builders::inspector_to_wire and the feed tests link both halves.
//
// SELECTION DRIVES THE FETCH (R-HUX-011). The Scene tree's selection listener calls `request()` /
// `request_clear()`; the owner loop's pump (`pump_panel_feeds`, builtin_panels.h) performs the RPC
// for the PENDING identity and hands the reply to `apply_result`. The feed itself performs NO IO —
// every function here is pure over its inputs and T1-testable on all three default `build` legs.
//
// WRITES GO OVER THE WIRE (M9 e09b-2, design 05 §7-§8). `inspector.edit` STAGES on the C++ model
// (the L-20 gesture-in-progress), and the gesture verb `commit` ends it: `InspectorPanel::commit`
// drives the L-30 rebase-or-drop engine through whatever `OverrideWriteGateway` was bound, which in
// the live Shell is the `WireOverrideWriteGateway` — the daemon's `edit` RPC with raw-byte CAS. A
// feed with NO gateway bound exposes NO gesture at all (`make_provider` leaves `PanelProvider::gesture`
// null, so the manifest reports `gestures:false` and `panel.gesture` refuses), rather than accepting a
// commit it could only swallow. That is the whole `gestures:false -> true` flip: the capability is
// REPORTED from what is actually bound, never stubbed.
//
// READ-YOUR-WRITES (05 §7) is wired here, at the ONE place that knows both the commit outcome and the
// identity to re-fetch: the feed registers a commit listener, and an applied/rebased commit
// re-requests its own identity so the next pump re-reads `editor.inspect` and the panel shows what it
// just wrote (with the field now `overridden`). The barrier that makes that read honest is the
// daemon's own — see `wire_override_gateway.h` § READ-YOUR-WRITES for why it is `--after-hash` inside
// `edit`, and not the inert reserved `--after-generation` core flag.
//
// THE LOUD DROP LANDED IN e09b-3. An L-30 drop reaches the listener, is COUNTED here
// (`drops_observed()` / `last_commit()`) — and now goes out through the NOTICE SINK below, which the
// composition root points at the Shell's `WriteNoticeRelay` (write_notice.h). That relay turns it
// into an `editor.ui` fact in every window, where editor-core's notification host renders it as a
// wait-hued toast: the three sinks design 05 §8 names ("drop LOUDLY + notification + editor.ui
// fact"). Counting a refusal and writing it to stderr — the state this feed shipped in until now — is
// indistinguishable from silence in a GUI, which is what design 10's non-negotiable "LOUD, never
// silent" invariant forbids.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/session/undo/undo_journal.h" // undo::FieldEdit (the checkpoint sink)
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace context::editor::shell::panels
{

namespace inspector = gui::panels::inspector;
// Redeclared alias (undo_feed.h declares the same one to the same namespace — legal, and either
// header may be included first).
namespace undo = gui::session::undo;

// --------------------------------------------------------------------------------- pure parsers

// The WidgetKind wire token table — MIRRORS builders::wire's widget_kind_token; keep in lockstep.
// Unknown tokens read as `readonly`, deliberately: a field whose editing affordance we cannot
// understand must not be offered for editing (fail-closed), while still being VISIBLE.
[[nodiscard]] inspector::WidgetKind parse_widget_kind(const std::string& token);

// Project one `editor.inspect` reply's `inspector` member back into the panel model. nullopt when
// `wire` is not an object or carries no `present` member — the one shape that says nothing. A
// `present: false` reply parses to an ENGAGED model with `has_entity == false` (the no-selection
// placeholder state): "nothing is selected there" is information, not absence of it.
[[nodiscard]] std::optional<inspector::InspectorModel> parse_inspector(const contract::Json& wire);

// The root scene file's raw-byte CAS token, carried as a DECIMAL string (the kernel_server rawHash
// discipline — a u64 exceeds 2^53 as a JSON number; serializer::parse_hash_string is its strict
// inverse). 0 for absent/garbage — which the panel treats as "no CAS guard", exactly the honest
// state for an unknown file.
[[nodiscard]] std::uint64_t parse_raw_hash(const std::string& text);

// One WHOLE `editor.inspect` reply -> the panel model + its CAS token. Resolves the shared envelope
// `data` level (wire_read.h), then THIS feed's own hop — prefer a nested `inspector` member over the
// bare data — and reads the sibling `rawHash` from the data level. `raw_hash` is ALWAYS written (0
// for absent/garbage), including when the model does not parse.
//
// Both consumers go through here — the feed's own `apply_result` AND the Shell's wire write gateway's
// L-30 re-read — deliberately: the value a rebase-or-drop decision compares must be byte-identical to
// the value the panel is showing, and a second hand-rolled hop is exactly how those two drift. Which
// key to look for is policy, so it stays in this file (see the WHERE THIS SITS note above), not in
// the TU-shared wire_read.h.
[[nodiscard]] std::optional<inspector::InspectorModel> parse_inspect_reply(const contract::Json& reply,
                                                                          std::uint64_t& raw_hash);

// ---------------------------------------------------------------- the node-id -> field mapping

// The prefix `InspectorPanel::build_panel` gives every editable widget
// (`inspector.widget.<pointer>`). Named here so the one dependency is greppable from both sides.
inline constexpr const char* kInspectorWidgetPrefix = "inspector.widget.";

// Resolve an activated NODE id to the field pointer `InspectorPanel::stage_edit` expects (the same
// host/runtime-agnosticism rationale as scenetree_row_identity / problems_row_identity). nullopt
// for a node that is not an inspector widget.
[[nodiscard]] std::optional<std::string> inspector_widget_pointer(const std::string& node_id);

// ------------------------------------------------------------------------------------ the feed

// Owns an InspectorPanel and drives it from the Scene tree's selection + the daemon's
// `editor.inspect` reads, touching the PanelHost on every change.
class InspectorFeed
{
public:
    // Non-owning: `host` must outlive the feed.
    InspectorFeed(PanelHost& host, std::string panel_id);

    InspectorFeed(const InspectorFeed&) = delete;
    InspectorFeed& operator=(const InspectorFeed&) = delete;
    InspectorFeed(InspectorFeed&&) = delete;
    InspectorFeed& operator=(InspectorFeed&&) = delete;

    // Selection moved -> a fetch for `identity` is pending (the pump performs it). A later request
    // REPLACES a pending one — only the latest selection matters.
    void request(const std::string& identity);

    // Selection cleared -> clear the panel NOW (no RPC needed to render the placeholder) and drop
    // any pending fetch.
    void request_clear();

    // Re-arm a re-read of the entity CURRENTLY inspected, because the file changed under the panel:
    // its own commit landed (READ-YOUR-WRITES, 05 §7) or an undo/redo replay wrote the field behind
    // its back (READ-YOUR-REPLAYS, e09c — a replay goes through the gateway directly, so `commit`
    // and therefore the commit listener never run). Returns false when nothing is inspected, which
    // is an ordinary state and not a failure.
    //
    // THE ONE HOME for that rule: `on_commit` and `pump_panel_feeds` both route through it, so
    // "which identity, and when is it empty" is not spelled out in two places that can drift.
    bool request_refresh();

    // The pump's contract (mirrors SceneTreeFeed): the pending identity to fetch, claimed via
    // `mark_fetched()` BEFORE the RPC so a failed call waits for the next selection change.
    [[nodiscard]] const std::optional<std::string>& pending() const noexcept { return pending_; }
    void mark_fetched() noexcept { pending_.reset(); }

    // Adopt one `editor.inspect` reply (envelope / bare-data tolerance, like the scenetree feed).
    // Adopts the model AND the rawHash CAS token via set_model. Returns true when adopted.
    bool apply_result(const contract::Json& reply);

    // Bind the L-20/L-30 write path (M9 e09b-2); `nullptr` detaches. MUST be called BEFORE
    // `make_provider()` — the manifest's `gestures` capability is decided at binding time and a
    // PanelHost refuses a second `provide()` for the same id, so a gateway arriving afterwards could
    // never flip the flag. The composition root (`install_builtin_panels`) does exactly that; the
    // gateway's own daemon connection is re-pointed independently, per frame
    // (`panels::bind_write_client`), so a reconnect never needs a re-bind here.
    void bind_gateway(const inspector::OverrideWriteGateway* gateway);
    [[nodiscard]] bool has_gateway() const noexcept { return gateway_bound_; }

    // The M9 e09c SESSION UNDO CHECKPOINT SINK. Where a resolved gesture commit goes so it becomes a
    // reversible undo step; the composition root points it at the `UndoFeed` (undo_feed.h). Erased
    // through a std::function — like EditorStateBridge's RegionSink — so this feed never names the
    // journal host, and so the dirty/persist policy lives in exactly one place (the UndoFeed), not
    // duplicated at this call site.
    //
    // ONLY AN APPLIED OR REBASED COMMIT IS SENT. A loudly-dropped one (L-30) wrote nothing, so there
    // is nothing to revert — recording it would offer the human an undo of an edit that never
    // landed, which is worse than offering none. An unbound sink is an ordinary state (a bag built
    // without a journal, as the T1 suites do): the commit still happens, it just is not journaled.
    using CheckpointSink = std::function<void(undo::FieldEdit)>;
    void bind_checkpoint_sink(CheckpointSink sink);
    [[nodiscard]] bool has_checkpoint_sink() const noexcept { return static_cast<bool>(sink_); }

    // The M9 e09b-3 LOUD WRITE-NOTICE SINK — where a REFUSED commit goes so the human is told.
    //
    // ONLY A DROP OR AN ERROR IS SENT, which is the exact complement of the checkpoint sink above:
    // that one takes the commits that LANDED, this one takes the commits that did not. An applied or
    // rebased commit is not a notice — the write is on disk and the panel re-reads it
    // (READ-YOUR-WRITES), so toasting it would train the human to ignore the channel that matters.
    //
    // Erased through a std::function for the same reason the checkpoint sink is: this feed must not
    // name `shell::WriteNoticeRelay`, so the notice TRANSPORT stays in the composition root and this
    // library keeps holding no store, no window set and no bridge. An unbound sink is an ordinary
    // state (every T1 bag builds one), and the drop is still counted and still reported to stderr.
    using NoticeSink = std::function<void(const inspector::CommitResult&)>;
    void bind_notice_sink(NoticeSink sink);
    [[nodiscard]] bool has_notice_sink() const noexcept { return static_cast<bool>(notice_sink_); }

    [[nodiscard]] inspector::InspectorPanel& panel() noexcept { return panel_; }
    [[nodiscard]] const inspector::InspectorPanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t results_applied() const noexcept { return results_applied_; }
    // Every resolved gesture commit the panel reported (applied / rebased / dropped / error).
    [[nodiscard]] std::size_t commits_observed() const noexcept { return commits_observed_; }
    // The L-30 LOUD drops among them — a concurrent writer touched the same field path, so the
    // gesture was refused rather than overwriting it. Since e09b-3 each one ALSO goes out through
    // `bind_notice_sink`; the COUNT is what makes a drop assertable without a renderer.
    [[nodiscard]] std::size_t drops_observed() const noexcept { return drops_observed_; }
    // How many refused commits (drop OR error) were handed to the notice sink (M9 e09b-3). Strictly
    // <= `commits_observed()`: an applied/rebased commit and a refusal with no sink bound are both
    // excluded. This is the count that makes "the human was told" assertable at T1 — without it a
    // regression that unbound the sink would look exactly like a run in which nothing was refused.
    [[nodiscard]] std::size_t notices_sent() const noexcept { return notices_sent_; }
    // How many times a re-read was armed by `request_refresh` — an applied/rebased commit
    // (read-your-writes, 05 §7) or, since e09c, a landed undo/redo replay (read-your-replays). NOT
    // selection changes, which arm through `request` directly.
    [[nodiscard]] std::size_t rereads_armed() const noexcept { return rereads_armed_; }
    // How many resolved commits were journaled as undo checkpoints (M9 e09c). Strictly <=
    // `commits_observed()`: a drop, an error, and a commit with no sink bound are all excluded.
    [[nodiscard]] std::size_t checkpoints_sent() const noexcept { return checkpoints_sent_; }
    [[nodiscard]] const inspector::CommitResult& last_commit() const noexcept
    {
        return last_commit_;
    }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding.
    // `inspector.edit` resolves the widget's field pointer and STAGES the supplied `value` (a JSON
    // literal string) on the C++ model; a dispatch carrying no parseable value is DECLINED — there
    // is nothing honest to stage.
    //
    // The GESTURE (04 §4's closed begin/extend/commit/cancel vocabulary) is bound ONLY when a gateway
    // was bound first — `commit` is the L-20 gesture end that actually writes, and a commit with
    // nowhere to write is the state this whole task exists to retire. No state pair either way
    // (reported absent, not stubbed).
    [[nodiscard]] PanelProvider make_provider();

private:
    // The commit listener body (registered on the panel at construction).
    void on_commit(const inspector::CommitResult& result);

    // Snapshot the in-flight gesture as a reversible undo checkpoint (M9 e09c). nullopt when nothing
    // is staged, or when the model carries no addressable target — a checkpoint whose `root_scene`
    // or `pointer` is empty addresses nothing, so its replay could only ever drop, and recording it
    // would put a permanently-unusable step on the human's undo stack.
    //
    // MUST BE CALLED BEFORE `InspectorPanel::commit()`, which consumes the staged edit (and with it
    // both the `before` and `after` values a checkpoint needs) — see inspector_panel.h's accessors.
    [[nodiscard]] std::optional<undo::FieldEdit> snapshot_checkpoint() const;

    PanelHost& host_;
    std::string panel_id_;
    inspector::InspectorPanel panel_;
    std::optional<std::string> pending_;
    std::size_t results_applied_ = 0;
    // Mirrors "a non-null gateway was bound": InspectorPanel exposes no gateway accessor, and
    // `make_provider` must answer the capability question without one.
    bool gateway_bound_ = false;
    std::size_t commits_observed_ = 0;
    std::size_t drops_observed_ = 0;
    std::size_t rereads_armed_ = 0;
    std::size_t checkpoints_sent_ = 0;
    std::size_t notices_sent_ = 0;
    CheckpointSink sink_;
    NoticeSink notice_sink_;
    inspector::CommitResult last_commit_;
};

} // namespace context::editor::shell::panels
