// The BOUNDED per-package EVENT FAN-OUT buffer (M9 e13c-2, design 04 §5 / 05 §1 / 08 §2).
//
// WHAT THIS IS. e13c-1 gave every package a BASELINE daemon session and one REQUEST/RESPONSE route
// onto it (`panel.daemon.call`). A subscription is the other direction: the daemon PUSHES `event`
// frames onto that same connection, at a rate the daemon chooses and the panel does not. Something on
// the Shell side has to hold them between the daemon's push and the panel's next drain, and THAT
// holder is the security-relevant object of this task:
//
//   ⚠ AN UNBOUNDED PER-PACKAGE BUFFER IS A MEMORY-EXHAUSTION VECTOR DRIVEN BY UNTRUSTED CODE. A
//   sandboxed package chooses WHAT to subscribe to (`subscribe` with no `topics` means every topic)
//   and chooses whether to ever drain. It does not have to send anything, produce load, or exploit a
//   bug: it subscribes broadly, stops polling, and the editor grows without bound for as long as the
//   project is being edited. That is exactly why `package_sessions.h`'s allowlist HELD `subscribe` /
//   `unsubscribe` / `ack` back until this file existed.
//
// THE BOUND, THE DROP POLICY, AND WHY THEY ARE THESE.
//
//   * CAPACITY is `kMaxBufferedEventsPerPackage` (256), PER PACKAGE — not per subscription and not
//     per panel. Per SUBSCRIPTION would let a package multiply its own bound by subscribing N times;
//     per PANEL would let it multiply the bound by opening N panels. The package is the identity the
//     session is pooled on (package_sessions.h § control 4), so it is the identity the memory is
//     budgeted on too, and a package cannot enlarge its own budget by any action available to it.
//
//   * THE DROP DISCIPLINE IS DROP-OLDEST (evict the front), NOT refuse-newest. Chosen because these
//     are FACTS, not commands: a panel drawing daemon state needs the CURRENT truth, and holding a
//     stale head while discarding the fresh tail would leave it rendering the past forever. It is
//     also what the daemon's own event ring already does to a slow subscriber, so the two layers do
//     not disagree about what "behind" means (client/subscription.h § 3).
//
//   * A DROP IS LOUD, NEVER SILENT (design 10; the e09b-3 precedent). Silence here would be the worst
//     possible failure: a panel that missed events looks EXACTLY like a panel whose subject did not
//     change, so it would render stale data indefinitely and blame itself. Three observables, and the
//     first is the one that reaches the package:
//       1. `PackageEventDrain::gapped` + `dropped` travel in the `panel.events.poll` REPLY and on to
//          the panel over its port. `gapped` is the instruction ("your cursor is worthless — take a
//          fresh snapshot"), which is precisely the recovery the daemon's own `event.gap` demands;
//          `dropped` is how many events THIS EDITOR discarded, so the two causes are distinguishable
//          rather than collapsed (see `mark_gap`).
//       2. `dropped_total()` — a Shell-side counter the T1 suite asserts on.
//       3. One stderr line per overflow EPISODE (not per event — a per-event line under overflow IS
//          the flood). Developer-facing only: stderr in a GUI is indistinguishable from silence
//          (write_notice.h says so), which is why observable 1 is the deliverable and this is a hint.
//
//   * `mark_gap` IS A DIFFERENT FACT FROM AN OVERFLOW, and both set `gapped`. The daemon sends
//     `event.gap` when ITS ring outran this client; the buffer overflows when the PANEL outran this
//     editor. The panel's correct response is identical (re-snapshot), so one flag carries it — but
//     `dropped` is non-zero only for the second, so a diagnosis can still tell which side was behind.
//     Collapsing them into one counter would make a daemon-side gap look like an editor-side leak.
//
// ⚠ NAMED RESIDUAL, NOT CLOSED HERE — THE SUBSCRIPTION COUNT. This file bounds the events a package
// may have IN FLIGHT; it does not bound how many SUBSCRIPTIONS a package may open. The daemon holds
// every minted subId until an explicit `unsubscribe` and fans each event out once PER subId
// (client/subscription.h § unsubscribe_one), so N subscriptions multiply the daemon's own per-client
// work even though this buffer's ceiling is unchanged. It is bounded in CONSEQUENCE here (a package
// with 100 subscriptions still holds at most 256 events on this side) and unbounded in the daemon's
// own bookkeeping. Closing it needs subId ownership tracking across `subscribe`/`unsubscribe`, which
// belongs with the grant store e13c-4 builds — recorded rather than papered over.
//
// CEF-FREE and D10 BOUNDARY-CLEAN like ui_mirror.h / write_notice.h: pure data movement over
// `contract::Json`, no browser, no window, no kernel-internal module — so `tests/test_package_events.cpp`
// drives the SAME buffer the real Shell runs, on all three default `build` legs.

#pragma once

#include "context/editor/contract/json.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The router method editor-core calls to DRAIN one package's buffered daemon events.
//
// Params `{packageId}`; result `{events:[...], dropped:<n>, gapped:<bool>}`. MIRRORED by the TS side
// (src/editor/webui/core/src/packageevents.ts `PANEL_EVENTS_POLL_METHOD`) — a rename on either side
// would leave editor-core polling a method the Shell no longer routes, and a package panel's events
// would silently stop arriving with NOTHING reporting it, exactly the regression session_bridge.h
// records for `session.state`.
inline constexpr const char* kPanelEventsPollMethod = "panel.events.poll";

// How many daemon events ONE package may hold undrained. See the header for why the budget is
// per-PACKAGE and why 256.
//
// Sized against the drain cadence rather than against a guess: editor-core polls on the same
// `REHOME_POLL_MS` tick as the rehome / drag / ui-mirror pumps, so 256 is far more than a healthy
// package accumulates between two ticks, and far below anything that matters in a process designed to
// stay up for days.
inline constexpr std::size_t kMaxBufferedEventsPerPackage = 256;

// The most frames ONE package's session may contribute to a SINGLE `PackageSessionHost::pump()`.
//
// The pump runs on the Shell's owner loop, so an unbounded drain would let a chatty daemon topic hold
// the frame — a responsiveness bound, distinct from the memory bound above. Anything still queued on
// the wire is simply drained by the next frame's pump.
inline constexpr std::size_t kMaxDrainedFramesPerPump = 64;

// What one drain handed back — the events, plus the LOUD half.
struct PackageEventDrain
{
    // The buffered events in daemon (push) order, oldest first.
    std::vector<contract::Json> events;
    // How many events THIS EDITOR discarded since the last drain (buffer overflow). 0 for a daemon
    // gap, which is what makes the two causes distinguishable — see the header.
    std::uint64_t dropped = 0;
    // Was anything lost at all (an overflow here, or an `event.gap` from the daemon)? TRUE means the
    // consumer's cursor is worthless and it must re-snapshot.
    bool gapped = false;
};

// The per-package mailbox of in-transit daemon events. ONE per app, owned by `PackageSessionHost`
// (which is what holds the sessions the events arrive on).
class PackageEventBuffer
{
public:
    // `capacity` 0 would mean "buffer nothing", which reads as a disabled feature rather than a
    // configuration error; it is clamped to 1 exactly as `PackageSessionHost` clamps its own cap.
    explicit PackageEventBuffer(std::size_t capacity = kMaxBufferedEventsPerPackage);

    // Append one daemon event for `package_id`, DROPPING THE OLDEST when the package is already at
    // capacity. The drop is counted and latches `gapped` until the next drain.
    void push(const std::string& package_id, contract::Json event);

    // Record a DAEMON-side gap (an `event.gap` frame) for `package_id`: the consumer's cursor is
    // worthless, but nothing was lost on THIS side, so `dropped` is untouched.
    void mark_gap(const std::string& package_id);

    // Read + CLEAR everything queued for `package_id`, oldest first, along with the loud half. An
    // unknown / empty package is an ordinary empty drain, not an error: polling before subscribing is
    // exactly what editor-core's unconditional tick does.
    [[nodiscard]] PackageEventDrain take(const std::string& package_id);

    // Drop everything held for a package that is going away (its session was reset), so a package
    // that never drains again does not hold its ceiling until app exit — the same hygiene
    // `UiMirrorStore::forget` provides for a destroyed window.
    void forget(const std::string& package_id);

    /** Forget every package. Idempotent — the `PackageSessionHost::reset` counterpart. */
    void clear();

    [[nodiscard]] std::size_t pending(const std::string& package_id) const;
    /** Events dropped for `package_id` since its last drain — the per-package half of observable 2. */
    [[nodiscard]] std::uint64_t dropped(const std::string& package_id) const;
    /** Events this buffer has EVER discarded. NON-ZERO IS A PANEL FALLING BEHIND, not a metric. */
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_total_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Queue
    {
        std::deque<contract::Json> events;
        std::uint64_t dropped = 0;
        bool gapped = false;
    };

    std::map<std::string, Queue> queues_;
    std::size_t capacity_;
    std::uint64_t dropped_total_ = 0;
};

} // namespace context::editor::shell
