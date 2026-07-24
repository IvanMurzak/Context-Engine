// The Shell-local CROSS-WINDOW MIRROR relay for the `editor.ui` bus (M9 e10d, design 05 §5, D7 tier 2).
//
// WHAT THIS IS, AND WHY IT HAS TO EXIST. e08c built the editor-core side of the cross-window mirror —
// `UiMirrorSink` (a sink to hand a locally-published `editor.ui` envelope OUT of a window) and
// `EditorUiBus.receiveMirrored` (a total entry for one arriving from another) — and unit-tested the
// ENVELOPE across two buses. But e08c deliberately wired NO transport between them and, its refine
// found, its two-bus ring drill terminated on the point-to-point loop breaker, so the OTHER loop
// breaker — the one a BROADCASTING transport needs (drop an envelope that arrives back at its own
// `origin`) — was never exercised end to end. This relay is that transport, and its shape is exactly
// the one that makes the branch fire: it BROADCASTS every published envelope to EVERY window INCLUDING
// the sender. The sender therefore receives its own envelope back and MUST drop it by `origin`; a
// unicast/point-to-point relay would never deliver it back and would leave the branch dark.
//
// D7 tier 2 — SHELL-LOCAL ONLY. The Shell mirrors chrome facts between ITS OWN windows; it never
// routes them onward to the daemon (that is the D7 violation `tools/check_ui_bus_boundary.py` exists
// to prevent). This relay holds envelopes in per-window queues and moves them window-to-window; there
// is no daemon path in it, by construction — the envelope is an OPAQUE `contract::Json` it never
// interprets, exactly like the D6 panel seed and the drag hover.
//
// CEF-FREE and D10 BOUNDARY-CLEAN, like window_bridge.h / cross_window_drag.h: pure data movement, no
// browser, no window, no bridge, no kernel-internal module — so `tests/test_ui_mirror.cpp` drives the
// SAME relay the real Shell runs, on all three default `build` legs. The FAN-OUT (which windows an
// envelope broadcasts to) lives with the `WindowBridge`, which already knows the live window set via
// its `WindowsProvider`; this store is only the per-window queues that fan-out feeds and each window
// drains, mirroring how `WindowMoveStore` holds the rehome queues the bridge enqueues into.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/window_registry.h" // WindowId

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace context::editor::shell
{

// The per-window mailbox of in-transit `editor.ui` envelopes. ONE per app (referenced by every
// window's `WindowBridge`), because an envelope published by window A must be readable by window B —
// a per-window store could not express a broadcast. Pure data movement; the envelope is carried
// verbatim and never interpreted.
class UiMirrorStore
{
public:
    // Append one broadcast envelope to `target`'s queue, delivered on that window's next poll. The
    // WindowBridge calls this once per live window (including the SENDER) for each publish — that
    // is the broadcast, and delivering to the sender too is what arms the receiving bus's own-origin
    // echo drop.
    void enqueue(WindowId target, contract::Json envelope)
    {
        queues_[target].push_back(std::move(envelope));
    }

    // Read + CLEAR every envelope queued for `target`, in enqueue (publish) order. Empty when none.
    [[nodiscard]] std::vector<contract::Json> take(WindowId target)
    {
        const auto it = queues_.find(target);
        if (it == queues_.end())
        {
            return {};
        }
        std::vector<contract::Json> pending = std::move(it->second);
        queues_.erase(it);
        return pending;
    }

    // Drop everything queued for a window that is going away, so a destroyed-and-never-drained target
    // does not leak its envelopes until app exit. Called by the app on `destroy_window`, exactly like
    // `WindowMoveStore::forget`.
    void forget(WindowId target) { queues_.erase(target); }

    [[nodiscard]] std::size_t pending(WindowId target) const
    {
        const auto it = queues_.find(target);
        return it == queues_.end() ? 0u : it->second.size();
    }

private:
    std::map<WindowId, std::vector<contract::Json>> queues_;
};

} // namespace context::editor::shell
