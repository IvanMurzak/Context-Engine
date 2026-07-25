// The SESSION UNDO HOST (M9 e09c, design 03 §1 / 05 §7-§8, R-HUX-001 / L-20 / L-21 / L-30): the
// Shell-side wiring that finally gives `gui/session/undo`'s journal a host.
//
// WHAT WAS MISSING. The journal shipped complete at M5-F7 — gesture checkpointing, CAS-guarded
// replay through the ONE L-30 engine, a JSON round-trip — and then sat inert: `to_json`/`load_json`
// were called by NO host, no host recorded a checkpoint, and no host could replay one. A journal
// nobody reads or writes is not short-horizon undo; it is dead code that LOOKS like undo. This file
// is the host, and it closes all three halves at once:
//
//   * RECORDS. The Inspector's resolved gesture commits arrive here as checkpoints (the composition
//     root wires `InspectorFeed`'s checkpoint sink at `record`). Only an APPLIED or REBASED commit
//     becomes a checkpoint — a loudly-dropped one wrote nothing, so there is nothing to revert.
//   * REPLAYS OVER THE WIRE. The gateway bound here is the SAME `WireOverrideWriteGateway` a live
//     gesture commits through, so undo/redo is the daemon's `edit` RPC with raw-byte CAS, the same
//     L-30 rebase-or-drop policy, and the same `--after-hash` barrier the daemon applies inside
//     `edit`. Replay is NOT a privileged path: a replayed write can hit `cas.mismatch` exactly like a
//     live one and is dropped LOUDLY rather than restoring stale bytes over a co-writer (R-HUX-001).
//     There is deliberately no second write path — the DoD's whole point.
//   * PERSISTS. The journal (de)serializes to a blob the Shell records into `.editor/editor-state.json`
//     through `EditorStateStore::set_undo` — the ONE Shell-side seam (C-F3 / e09d), reusing e05d2's
//     existing store rather than introducing a second serializer or a parallel file. So the journal
//     survives an editor restart: boot loads the blob back and Ctrl+Z still works on the previous
//     session's edits.
//
// WHY THE BLOB IS A CANONICAL STRING. `editor_state.h` § EditorState::undo states it: the journal's
// DOM is `serializer::JsonValue` and the store's is `contract::Json`, and the latter holds every
// number as a `double` — so a nested-object conversion would round-trip user data through a lossy
// hop. The transport is therefore `serialize_canonical` bytes (R-FILE-001, the engine's one value
// identity) and the ONE journal serializer stays `UndoJournal::to_json`.
//
// TOTAL AND DISK-FREE, like the sibling feeds: every function here is pure over its inputs, opens no
// file, and is T1-testable on all three default `build` legs with no CEF and no daemon.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway
#include "context/editor/gui/session/undo/undo_journal.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <string>

namespace context::editor::shell::panels
{

// Redeclared alias (inspector_feed.h declares the same one to the same namespace, which is legal and
// deliberate — either header may be included first).
namespace inspector = gui::panels::inspector;
// The journal's namespace. NOTE the member functions below are `replay_undo` / `replay_redo`, NOT
// `undo` / `redo`: a member named `undo` would HIDE this alias inside the class body, so every
// `undo::` there would fail to compile. Naming them after what they do keeps the alias reachable.
namespace undo = gui::session::undo;

// --------------------------------------------------------------------------- the persistence blob
//
// Pure, and the inverse of each other. Exposed as free functions (not just members) so the T1 suite
// can assert the round-trip WITHOUT a PanelHost — the property that matters is the transport's, not
// the feed's.

// The journal's canonical serialization, as the string blob `EditorState::undo` carries. An empty
// string when the journal could not be serialized at all (which `serialize_canonical` only reports
// for a value outside the canonical form — never for a journal this build produced).
[[nodiscard]] contract::Json undo_journal_to_blob(const undo::UndoJournal& journal);

// Replace `journal`'s stacks from a blob. False — with the journal left EMPTY — for a blob that is
// not a non-empty string, does not parse, or is not a journal document. A corrupt session file
// costs the undo history, never the boot (the `UndoJournal::load_json` contract, honestly
// propagated).
[[nodiscard]] bool undo_journal_from_blob(const contract::Json& blob, undo::UndoJournal& journal);

// ------------------------------------------------------------------------------------- the feed

class UndoFeed
{
public:
    // Non-owning: `host` must outlive the feed (the provider below captures `this`).
    UndoFeed(PanelHost& host, std::string panel_id);

    // Non-copyable AND non-movable, like every sibling feed: `make_provider` captures `this`, and the
    // Inspector's checkpoint sink captures a raw `UndoFeed*`, so a relocatable object would be a
    // use-after-free waiting to happen.
    UndoFeed(const UndoFeed&) = delete;
    UndoFeed& operator=(const UndoFeed&) = delete;
    UndoFeed(UndoFeed&&) = delete;
    UndoFeed& operator=(UndoFeed&&) = delete;

    // Bind the write path replay goes through; `nullptr` detaches. In the live Shell this is the
    // `WireOverrideWriteGateway` — the SAME object the Inspector's gesture commits through, which is
    // what makes replay "the same write path" structurally rather than by convention. The gateway
    // must OUTLIVE this feed (the journal holds a raw pointer to it); the composition root's member
    // order is what guarantees that.
    //
    // An UNBOUND feed is not a silent no-op: `UndoJournal::undo`/`redo` answer `Status::none`, the
    // provider reports the command as not dispatched, and nothing is recorded as replayed.
    void bind_gateway(const inspector::OverrideWriteGateway* gateway);
    [[nodiscard]] bool has_gateway() const noexcept { return gateway_bound_; }

    [[nodiscard]] undo::UndoJournal& journal() noexcept { return journal_; }
    [[nodiscard]] const undo::UndoJournal& journal() const noexcept { return journal_; }

    // Record ONE committed field edit as a lone gesture checkpoint (L-20's one-undo-step-per-gesture,
    // which is what the Inspector produces — one field per gesture). Dirties the persistence blob and
    // touches the panel so the surface re-renders with the new depth.
    void record(undo::FieldEdit edit);

    // Ctrl+Z / Ctrl+Y. Named `replay_*` for the namespace-alias reason above. Both dirty the blob
    // whenever the stacks actually moved — including a DROPPED replay, which still pops the
    // checkpoint (a checkpoint whose field a co-writer has since moved can never be replayed, so
    // keeping it would offer the human an undo that is guaranteed to refuse).
    undo::ReplayResult replay_undo();
    undo::ReplayResult replay_redo();

    // --- persistence (the C-F3 single-writer seam's payload) --------------------------------------

    // The blob to hand `EditorStateStore::set_undo`.
    [[nodiscard]] contract::Json to_blob() const;
    // Adopt a blob read back from the store (boot / restart). Returns what
    // `undo_journal_from_blob` did; either way the feed is CLEAN afterwards — it now matches disk.
    bool load_blob(const contract::Json& blob);

    // Whether the journal has moved since the last `to_blob` publish (see `mark_clean`). The owner
    // loop gates its per-frame publish on this so an idle editor re-serializes nothing.
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void mark_clean() noexcept { dirty_ = false; }

    // --- what it saw (the T1/T2 assertion surface) ------------------------------------------------
    [[nodiscard]] std::size_t checkpoints_recorded() const noexcept { return checkpoints_recorded_; }
    // How many replays actually ran (a `none` — nothing to undo / no gateway — does not count).
    [[nodiscard]] std::size_t replays_run() const noexcept { return replays_run_; }
    // How many of those were LOUD DROPS: a co-writer moved the field, so the replay refused rather
    // than restoring stale bytes. The count is what makes the R-HUX-001 guarantee assertable.
    [[nodiscard]] std::size_t replay_drops() const noexcept { return replay_drops_; }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding.
    // `build` renders the journal's own headless a11y-clean panel; `invoke` dispatches the two
    // commands that panel exposes (`session.undo` / `session.redo`), which is how the R-CLI-001
    // keyboard/CLI path reaches the replay. No gesture (undo has no continuous geometry) and no D6
    // state pair — the journal's state is persisted through the store, not through the panel-state
    // channel, precisely because the Shell owns it and editor-core must not.
    [[nodiscard]] PanelProvider make_provider();

private:
    PanelHost& host_;
    std::string panel_id_;
    undo::UndoJournal journal_;
    // Mirrors "a non-null gateway was bound": UndoJournal exposes no gateway accessor.
    bool gateway_bound_ = false;
    bool dirty_ = false;
    std::size_t checkpoints_recorded_ = 0;
    std::size_t replays_run_ = 0;
    std::size_t replay_drops_ = 0;
};

} // namespace context::editor::shell::panels
