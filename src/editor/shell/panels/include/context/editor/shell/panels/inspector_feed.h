// The LIVE inspector feed for the Inspector panel (M9 e05d3, design 04 §4 / 05 §7) — the projection
// from the daemon's `editor.inspect` composed read onto the headless `InspectorPanel` model, plus
// the `PanelProvider` that publishes it through the Shell's panel host.
//
// WHERE THIS SITS. The Shell is an ordinary client (D10/D18): the schema-driven field derivation
// runs on the daemon (gui/panels/builders/, served by `editor.inspect`), and what arrives here is
// the boundary-clean panel model AS DATA — plus the root scene's raw-byte CAS token, adopted as the
// panel's base hash so a future wire write (e09's WireOverrideWriteGateway) guards on live state.
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
// A LOUD DROP IS NOT THIS TASK'S (e09b-3). An L-30 drop reaches the listener and is COUNTED here
// (`drops_observed()` / `last_commit()`); the human-visible notification host, the `editor.ui`
// notification topic and the wait-hue surface are e09b-3. A drop with no chrome yet is an honest
// state — the write was refused rather than silently overwriting a concurrent editor's value, which
// is the guarantee that actually protects the human's data.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace context::editor::shell::panels
{

namespace inspector = gui::panels::inspector;

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

    [[nodiscard]] inspector::InspectorPanel& panel() noexcept { return panel_; }
    [[nodiscard]] const inspector::InspectorPanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t results_applied() const noexcept { return results_applied_; }
    // Every resolved gesture commit the panel reported (applied / rebased / dropped / error).
    [[nodiscard]] std::size_t commits_observed() const noexcept { return commits_observed_; }
    // The L-30 LOUD drops among them — a concurrent writer touched the same field path, so the
    // gesture was refused rather than overwriting it. The human-visible surface is e09b-3; the COUNT
    // is what makes a drop assertable today instead of invisible.
    [[nodiscard]] std::size_t drops_observed() const noexcept { return drops_observed_; }
    // How many applied/rebased commits re-armed the read-your-writes fetch (05 §7).
    [[nodiscard]] std::size_t rereads_armed() const noexcept { return rereads_armed_; }
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
    inspector::CommitResult last_commit_;
};

} // namespace context::editor::shell::panels
