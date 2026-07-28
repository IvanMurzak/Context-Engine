// The LOUD WRITE-NOTICE relay (M9 e09b-3, design 05 §8, 10 § Non-negotiable UX invariants).
//
// WHAT THIS CLOSES. e09b-1 put pointer/value CAS on the daemon's `edit` verb and e09b-2 made the
// editor commit through it (`WireOverrideWriteGateway`), so an L-30 collision now DROPS the gesture
// rather than clobbering a co-writer's value. That is the guarantee that protects the human's data —
// but until this file the human was never TOLD. The drop was counted (`InspectorFeed::drops_observed`,
// `UndoFeed::replay_drops`) and written to stderr, which in a GUI is indistinguishable from silence.
// Design 05 §8 states the requirement exactly: "drop LOUDLY + notification + editor.ui fact", and
// design 10 makes it non-negotiable: "Destructive/lossy moments (gesture drop, daemon lost, panel
// crash) are LOUD (wait/bad hues), never silent (L-30 discipline surfaced in UI)".
//
// WHY IT RIDES THE `ui.mirror` RELAY RATHER THAN A NEW BRIDGE METHOD. A notice must travel C++ -> TS,
// and the e05c bridge accepts NO persistent queries (every query completes inside `OnQuery`), so the
// only shape available is a method editor-core POLLS. The editor already has exactly one such
// Shell-to-renderer push path — `ui.mirror` / `ui.mirror-poll` (window_bridge.h, e10d) — whose queues
// carry OPAQUE `{seq, topic, origin, payload}` `editor.ui` envelopes the Shell never interprets. A
// notice IS an `editor.ui` fact (05 §8 says so in those words), so it is published as one INTO that
// relay instead of cloning a second poll surface. Three things fall out, and each is worth more than
// the small semantic stretch of publishing Shell-side onto a bus that is otherwise renderer-owned:
//
//   * ZERO new wire methods, so the nine live `editor-cef-smoke-shell*` scenarios need no edit. A new
//     boot-time method would be an `unknown_method` REFUSAL in every smoke that did not install it,
//     tripping their strict `bridge.refused() == 0` invariant (the regression e06d shipped and
//     session_bridge.h records) — nine files of blast radius for a second copy of one relay.
//   * The BROADCAST is already right. `UiMirrorStore` is per-window keyed and every window drains its
//     own queue, so one notice reaches EVERY open window. A refused write is exactly the kind of fact
//     that must not be visible in only one of them.
//   * `receiveMirrored` (uibus.ts) already re-seals, retains and fans out an arriving envelope, so the
//     `editor.ui` FACT sink of the DoD is satisfied by the transport itself rather than by a parallel
//     mechanism that could drift from it.
//
// THE ORIGIN IS `shell`, NEVER A WINDOW ID, and that is load-bearing rather than cosmetic.
// `EditorUiBus.receiveMirrored` DROPS an envelope whose `origin` equals the receiving bus's own
// origin — the broadcasting loop breaker — and each window's bus origin is its numeric window id
// (`String(windowId)`, boot.ts). A notice stamped with a window id would therefore be silently
// swallowed by exactly the window it was most likely meant for. `kWriteNoticeOrigin` is not a window
// id and can never collide with one.
//
// D7 IS UNTOUCHED. This is tier-2 chrome moving Shell -> its own windows; nothing here reaches the
// daemon, and `tools/check_ui_bus_boundary.py` still holds the renderer half down.
//
// CEF-FREE and D10 BOUNDARY-CLEAN like ui_mirror.h / window_bridge.h: pure data movement over
// `contract::Json`, no browser, no window, no kernel-internal module — so `tests/test_write_notice.cpp`
// drives the SAME relay the real Shell runs, on all three default `build` legs.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h" // WindowId

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The SEVENTH built-in `editor.ui` topic (uibus.ts `UI_TOPIC_WRITE_NOTICE`).
//
// ⚠ THE BUILT-IN TOPIC SET IS CLOSED (05 §5, enforced by `tools/check_ui_bus_boundary.py` and
// `uibus.test.ts`), so adding to it is a deliberate act and needs an authority. It has one: 05 §8's
// canonical flow ends `drop LOUDLY + notification + editor.ui fact`, which REQUIRES a topic to carry
// that fact, and §5's own list predates §8's requirement rather than excluding it. The set stays
// closed — this is a seventh member of it, not a door left open — and `EditorUiBus.publish` still
// refuses any `editor.ui.*` name nobody declared.
//
// Cross-checked byte-for-byte against the TS constant out of the BUILT bundle by
// `tools/check_webui_assets.py --panel-contract` (ctest `webui-panel-contract`), like every other
// vocabulary that crosses this language boundary: a rename on one side would leave the Shell
// broadcasting a topic editor-core's bus does not know, `receiveMirrored` would refuse it as an
// unknown topic, and every drop notice would silently stop reaching the human — the exact failure
// this task exists to end, restored by a typo.
inline constexpr const char* kUiTopicWriteNotice = "editor.ui.write-notice";

// The `origin` stamped on a Shell-published envelope. See § THE ORIGIN IS `shell` above for why this
// must not be a window id.
inline constexpr const char* kWriteNoticeOrigin = "shell";

// The three notice KINDS, and the whole reason they are distinguished: they mean different things to
// the human and therefore take different hues (06 §2 binds the hues 1:1 to semantics).
//
//   * DROP — the L-30 rebase-or-drop engine refused the write because a CONCURRENT WRITER moved this
//     same field. Nothing was lost and nothing was overwritten; the human's edit simply did not land
//     and must be re-made against the value that is there now. That is `wait` — 06 §2's
//     "awaiting-human" — not an error.
//   * REFUSAL — the write PATH refused (no daemon, an unreadable field, a compose refusal). Nothing
//     was written and no concurrency event was observed. That is `bad`.
//   * ABANDONED (M9 x10, CE #452) — NO WRITE WAS EVER ATTEMPTED. A staged Inspector gesture was
//     destroyed because the panel had to adopt an incoming model it could not withhold — the shared
//     selection moved under an in-flight edit, OR the same entity was re-read from disk under one
//     (`InspectorFeed::apply_result`, which is why the renderer's headline names no single cause and
//     the Shell's `message` supplies the specific one). It is a THIRD kind and
//     not a re-use of DROP, because DROP's whole sentence is a lie here: nobody "changed that field
//     first", there was no CAS, and no concurrent writer need exist at all. Telling the human to
//     "re-apply against the current value" when the field they were editing is no longer even on
//     screen sends them looking for a collision that never happened. Hue is `wait`, like DROP and for
//     06 §2's stated reason (`awaiting human`): the write path is healthy and the thing to do IS to
//     re-make the edit — which is exactly what `bad` ("wait for the project to be reachable") would
//     mis-state.
//
// All three are pinned against their TS twins by the `webui-panel-contract` gate: a drift makes every
// notice fall through to the renderer's unknown-kind default, which would silently mis-hue a
// data-integrity moment.
inline constexpr const char* kWriteNoticeKindDrop = "drop";
inline constexpr const char* kWriteNoticeKindRefusal = "refusal";
inline constexpr const char* kWriteNoticeKindAbandoned = "abandoned";

// One refused write — or, since M9 x10, one ABANDONED gesture — in the shape the renderer renders.
//
// `action` is FREE TEXT the Shell composes ("edit", "undo", "redo") and the renderer only displays,
// deliberately: it crosses the boundary as prose rather than as a token, so it needs no pin and a
// new caller cannot introduce a silent drift by inventing one.
struct WriteNotice
{
    // `kWriteNoticeKindDrop`, `kWriteNoticeKindRefusal` or `kWriteNoticeKindAbandoned`. Defaulted to
    // REFUSAL to agree with every
    // other unknown-input path in this feature (`notice_kind_for`, and `writeNoticeTone` in
    // notifications.ts): over-stating severity costs the human a second look, while defaulting to the
    // gentle "awaiting you" hue would send them re-applying an edit that may not be able to land.
    // Every producer assigns this explicitly; the default only decides what a hand-built notice means.
    std::string kind = kWriteNoticeKindRefusal;
    // What the human tried to do, for the message ("edit" / "undo" / "redo").
    std::string action;
    // The catalog code the write path answered with (`cas.mismatch`, `shell.no_daemon`, `compose.*`) —
    // or, for an ABANDONED gesture, the host-minted `panels::kGestureAbandonedCode`, which is
    // deliberately not a catalog entry because no daemon verb was called at all (inspector_feed.h).
    std::string code;
    // The human/AI-readable detail the L-30 engine or the write path produced.
    std::string message;
    // The field pointer the refused write — or the abandoned gesture — targeted. Empty when the caller
    // had none.
    std::string pointer;
};

// Build the `{seq, topic, origin, payload}` mirror envelope for one notice.
//
// PURE and separately exposed so the T1 suite can assert the wire shape without a store — the same
// discipline the `WindowBridge` method bodies follow. `seq` is the RELAY's own counter; the receiving
// bus re-seals with its own anyway (uibus.ts `receiveMirrored`), so this one exists to make the
// Shell's publish order legible in a log, not to order anything on the renderer side.
[[nodiscard]] contract::Json write_notice_envelope(const WriteNotice& notice, std::uint64_t seq);

// The relay: turn a refused write into an `editor.ui` fact in every live window's mirror queue.
//
// UNBOUND IS AN HONEST, SUPPORTED STATE (the `bind_drag_store` / `bind_ui_mirror_store` discipline):
// with no store, `publish` reaches nobody and answers 0. That is what a build with no mirror session
// gets — the counters still move, so a test can tell "nothing was published" from "published to
// nobody", and no caller has to know whether the transport exists.
class WriteNoticeRelay
{
public:
    // The live window set. Without one, `publish` still reaches `kPrimaryWindowId` — a single-window
    // editor must not lose its notices merely because nobody bound an enumerator.
    using WindowsProvider = std::function<std::vector<WindowId>()>;

    WriteNoticeRelay() = default;

    // Non-copyable and non-movable, like every sibling relay: the composition root hands out
    // std::functions that capture it and a router outlives nothing that could be relocated.
    WriteNoticeRelay(const WriteNoticeRelay&) = delete;
    WriteNoticeRelay& operator=(const WriteNoticeRelay&) = delete;
    WriteNoticeRelay(WriteNoticeRelay&&) = delete;
    WriteNoticeRelay& operator=(WriteNoticeRelay&&) = delete;

    void bind_store(UiMirrorStore* store) noexcept { store_ = store; }
    void bind_windows(WindowsProvider provider) { windows_ = std::move(provider); }
    [[nodiscard]] bool has_store() const noexcept { return store_ != nullptr; }

    // Broadcast `notice` to every live window. Returns how many windows it was queued for (0 when no
    // store is bound).
    //
    // A throwing WindowsProvider is CONTAINED — it degrades to the primary window instead of taking
    // down a write path whose whole job at that moment is to fail gracefully. That containment is the
    // guarantee; this is deliberately NOT documented as `noexcept`, because building the envelope and
    // enqueueing it still allocate, so an allocation failure does propagate. Claiming otherwise would
    // invite a caller to treat it as a `noexcept` boundary it is not.
    std::size_t publish(const WriteNotice& notice);

    // --- what it saw (the T1/T2 assertion surface) -----------------------------------------------

    // How many notices were published (including those that reached nobody, so a missing store is
    // distinguishable from a caller that never fired).
    [[nodiscard]] std::size_t published() const noexcept { return published_; }
    // How many per-window envelopes were queued in total.
    [[nodiscard]] std::size_t delivered() const noexcept { return delivered_; }
    // The last assigned envelope seq (0 before the first publish).
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

private:
    UiMirrorStore* store_ = nullptr;
    WindowsProvider windows_;
    std::size_t published_ = 0;
    std::size_t delivered_ = 0;
    std::uint64_t seq_ = 0;
};

} // namespace context::editor::shell
