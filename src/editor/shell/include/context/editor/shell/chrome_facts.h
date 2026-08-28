// The Shell-published CHROME facts (editor-window-chrome a1, target design 02 §1) — today exactly
// one: a window's `maximized` state flipped.
//
// WHAT THIS CLOSES. The a2 titlebar renders a max/restore glyph, and the state that glyph shows is
// OS truth the renderer cannot observe: a maximize can come from the button, from a double-click on
// the caption, from Win+Up, from the WM. The Shell already watches that truth — the 250 ms placement
// poll (shell.cpp § poll_placement) detects every placement change, maximized included — so 02 §1
// binds the channel: publish the flip as a fact on the EXISTING `editor.ui` mirror relay. No new
// push channel, no extra poll, no new wire method; `WindowManager::on_chrome_maximized` reports the
// flip and this relay turns it into an envelope in the window's mirror queue, which editor-core
// drains on its existing `ui.mirror-poll` and feeds to `EditorUiBus.receiveMirrored`.
//
// THE PRECEDENT IS write_notice.h, deliberately, down to the section names: an EIGHTH built-in
// `editor.ui` topic (the set stays CLOSED — this is one more declared member with a design
// authority, 02 §1's "changes are broadcast as a fact on the existing editor.ui mirror relay", not
// a door left open), the `shell` origin that can never collide with a window id (a fact stamped
// with the receiving window's own id would be swallowed by its bus's own-origin echo drop), and the
// `webui-panel-contract` byte-compare against the TS constant out of the BUILT bundle.
//
// UNICAST, WHERE THE WRITE NOTICE BROADCASTS — and the difference is the payload's meaning. A
// refused write is app-wide news every window must show; a maximized flip is a fact about ONE
// window's chrome, rendered only by that window's own titlebar glyph. So the relay enqueues into
// the affected window's queue alone, and the payload still carries `windowId` so a consumer never
// has to infer whose chrome changed from which queue it drained.
//
// D7 IS UNTOUCHED: tier-2 chrome moving Shell -> its own window; nothing here reaches the daemon.
// CEF-FREE and D10 BOUNDARY-CLEAN like ui_mirror.h / write_notice.h: pure data movement over
// `contract::Json`, so tests/test_chrome_facts.cpp drives the SAME relay the real Shell runs, on
// all three default `build` legs.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h" // WindowId

#include <cstddef>
#include <cstdint>

namespace context::editor::shell
{

// The EIGHTH built-in `editor.ui` topic (uibus.ts `UI_TOPIC_CHROME`). Cross-checked byte-for-byte
// against the TS constant out of the BUILT bundle by `tools/check_webui_assets.py --panel-contract`
// (ctest `webui-panel-contract`), like every vocabulary that crosses the language boundary: a drift
// makes `receiveMirrored` refuse every chrome fact as an unknown topic, and the a2 glyph silently
// stops flipping — restored by a typo, with both builds green.
inline constexpr const char* kUiTopicChrome = "editor.ui.chrome";

// The `origin` stamped on a Shell-published chrome envelope. The same value — and the same
// load-bearing rationale — as write_notice.h § THE ORIGIN IS `shell`: each window's bus drops an
// envelope whose origin equals its own (the broadcasting loop breaker), and a window's bus origin
// is its numeric window id, so a fact stamped with the target window's id would be swallowed by
// exactly the window it is for. `shell` is not a window id and can never collide with one.
inline constexpr const char* kChromeFactOrigin = "shell";

// Build the `{seq, topic, origin, payload}` mirror envelope for one maximized flip. PURE and
// separately exposed so the T1 suite asserts the wire shape without a store — the same discipline
// write_notice_envelope follows. Payload: `{windowId, maximized}`, both written unconditionally.
[[nodiscard]] contract::Json chrome_maximized_envelope(WindowId window, bool maximized,
                                                       std::uint64_t seq);

// The relay: turn an observed maximized flip into an `editor.ui` fact in the affected window's
// mirror queue.
//
// UNBOUND IS AN HONEST, SUPPORTED STATE (the write_notice / bind_ui_mirror_store discipline): with
// no store, `publish_maximized` reaches nobody and answers 0, while `published()` still moves — so
// a test can tell "no flip was reported" from "reported to nobody", and no caller has to know
// whether the transport exists.
class ChromeFactRelay
{
public:
    ChromeFactRelay() = default;

    // Non-copyable and non-movable, like every sibling relay: the composition root hands the
    // WindowManager a sink that captures it, and nothing that a std::function captured may be
    // relocated out from under it.
    ChromeFactRelay(const ChromeFactRelay&) = delete;
    ChromeFactRelay& operator=(const ChromeFactRelay&) = delete;
    ChromeFactRelay(ChromeFactRelay&&) = delete;
    ChromeFactRelay& operator=(ChromeFactRelay&&) = delete;

    void bind_store(UiMirrorStore* store) noexcept { store_ = store; }
    [[nodiscard]] bool has_store() const noexcept { return store_ != nullptr; }

    // Queue "window `window`'s maximized state is now `maximized`" for that window's next
    // `ui.mirror-poll`. Returns how many queues it reached (1, or 0 with no store bound).
    std::size_t publish_maximized(WindowId window, bool maximized);

    // --- what it saw (the T1 assertion surface) --------------------------------------------------
    // How many flips were published (including those that reached nobody — see the class note).
    [[nodiscard]] std::size_t published() const noexcept { return published_; }
    // How many envelopes were actually queued.
    [[nodiscard]] std::size_t delivered() const noexcept { return delivered_; }
    // The last assigned envelope seq (0 before the first publish).
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

private:
    UiMirrorStore* store_ = nullptr;
    std::size_t published_ = 0;
    std::size_t delivered_ = 0;
    std::uint64_t seq_ = 0;
};

} // namespace context::editor::shell
