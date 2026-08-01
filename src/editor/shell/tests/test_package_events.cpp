// T1 for the BOUNDED per-package event fan-out buffer + `panel.events.poll` (M9 e13c-2,
// design 04 §5 / 05 §1 / 08 §2).
//
// WHAT THIS PROVES AND WHY EACH HALF MATTERS. The buffer is a SECURITY control — the thing standing
// between a sandboxed package's subscription and an unbounded allocation in a process designed to stay
// up for days — so the properties pinned here are the ones whose failure is a denial-of-service rather
// than a bug:
//
//   * THE CAP IS DRIVEN PAST, NOT DESCRIBED. `the_cap_is_real_*` pushes `capacity + N` events and
//     asserts the EXACT surviving seq list (the LAST `capacity`), not merely that the size is bounded.
//     A size-only assertion cannot tell drop-OLDEST from drop-NEWEST, and the two are opposite
//     behaviours: one leaves a panel rendering current truth, the other leaves it rendering the past
//     forever.
//
//   * BOTH DIRECTIONS OF THE LOUD CLAIM. The DoD phrase is "a drop is observable rather than silent",
//     which is an assertion about two states, so both are pinned in the SAME fixture family:
//     `under_the_cap_*` proves a NON-overflowing drain reports `gapped == false` / `dropped == 0` AND
//     delivers every event, and `the_cap_is_real_*` proves an overflowing one reports `gapped == true`
//     / `dropped == N`. Without the first, "gapped is true after an overflow" could hold because
//     `gapped` is simply always true, and no plant against the drop path could tell.
//
//   * THE TWO CAUSES OF `gapped` STAY DISTINGUISHABLE. A daemon `event.gap` and an editor-side
//     overflow both mean "re-snapshot", so they share the flag — but only the second sets `dropped`.
//     `a_daemon_gap_*` asserts the POSITIVE artifact (`dropped == 0` WITH `gapped == true` and the
//     events still present), which an absence-only assertion could not distinguish from "nothing ever
//     arrived".
//
//   * THE PUMP IS WHAT MAKES THE BOUND REAL. Events arrive in `Client::pending_events_`, an UNBOUNDED
//     deque, so a buffer filled only when the renderer polls would leave the real accumulation point
//     unbounded. `the_pump_*` drives a REAL `client::Client` over a scripted wire (mock_channel.h's
//     standing warning: the mock is at the WIRE, so a real Client parses real dispatcher-shaped
//     frames) and asserts the events land in THIS package's buffer and in no other's.

#include "context/editor/shell/package_events.h"

#include "context/editor/contract/handshake.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/package_sessions.h"

#include "mock_channel.h"
#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace shell = context::editor::shell;

using contract::Json;
using shell::BridgeRouter;
using shell::PackageEventBuffer;
using shell::PackageEventDrain;
using shell::PackageSessionHost;

// The two fixture packages. Both are grammatically valid ids (`is_valid_package_id`), so every
// refusal below comes from the control under test and never from the id parser.
constexpr const char* kPkgA = "hello-panel";
constexpr const char* kPkgB = "other-panel";

/** The `seq` of every event in a drain, in delivery order — the artifact the ordering claims read. */
[[nodiscard]] std::vector<std::uint64_t> seqs_of(const PackageEventDrain& drain)
{
    std::vector<std::uint64_t> out;
    out.reserve(drain.events.size());
    for (const Json& event : drain.events)
    {
        out.push_back(event.contains("seq") && event.at("seq").is_number()
                          ? static_cast<std::uint64_t>(event.at("seq").as_int())
                          : 0u);
    }
    return out;
}

/** One wire event envelope carrying `seq` — the shape `clientmock::make_event` builds. */
[[nodiscard]] Json event_with_seq(std::uint64_t seq)
{
    return clientmock::make_event(seq, "inc-1", "diagnostics");
}

// ----------------------------------------------------- 1. the bound, driven from BOTH directions

// THE POSITIVE HALF. A drain that did NOT overflow must deliver EVERY event, in order, and report
// itself clean — otherwise "gapped after an overflow" proves nothing (the flag could be constant).
void under_the_cap_every_event_is_delivered_in_order_and_nothing_is_reported_lost()
{
    PackageEventBuffer buffer(8);
    for (std::uint64_t seq = 1; seq <= 8; ++seq)
    {
        buffer.push(kPkgA, event_with_seq(seq));
    }
    CHECK(buffer.pending(kPkgA) == 8);

    const PackageEventDrain drain = buffer.take(kPkgA);
    CHECK(drain.events.size() == 8);
    // The EXACT list, not a count: an ordering regression is invisible to a size assertion.
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(drain.dropped == 0);
    CHECK(!drain.gapped);
    CHECK(buffer.dropped_total() == 0);
}

// THE NEGATIVE HALF, AND THE DoD's OWN SENTENCE: "a test that actually drives the buffer past its
// cap". `capacity + 5` events go in; the survivors must be the LAST `capacity`, which is what
// distinguishes drop-OLDEST from drop-NEWEST — a bound alone cannot.
void the_cap_is_real_and_the_oldest_events_are_the_ones_dropped()
{
    PackageEventBuffer buffer(8);
    for (std::uint64_t seq = 1; seq <= 13; ++seq)
    {
        buffer.push(kPkgA, event_with_seq(seq));
    }
    // Never MORE than the cap, at any moment — the memory claim itself.
    CHECK(buffer.pending(kPkgA) == 8);

    const PackageEventDrain drain = buffer.take(kPkgA);
    CHECK(drain.events.size() == 8);
    // seqs 1..5 were evicted; 6..13 survive. Written out rather than computed so a change to the
    // discipline cannot be absorbed by the arithmetic in the assertion.
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({6, 7, 8, 9, 10, 11, 12, 13}));
    // THE LOUD PAIR — the ONLY channel that reaches the package.
    CHECK(drain.dropped == 5);
    // NAMED so a plant's RED is ATTRIBUTABLE: `CHECK(drain.gapped)` prints its own expression, and
    // three cases would otherwise print the identical line for three different claims.
    const bool overflow_reported_gapped = drain.gapped;
    CHECK(overflow_reported_gapped);
    // …and the Shell-side observable.
    CHECK(buffer.dropped_total() == 5);
}

// A DAEMON gap and an EDITOR overflow both mean "re-snapshot" and share the flag, but only the second
// lost anything HERE. Asserted as a POSITIVE artifact (the events are still present, and `dropped` is
// exactly 0) so this cannot pass because nothing ever arrived.
void a_daemon_gap_sets_gapped_without_claiming_this_editor_dropped_anything()
{
    PackageEventBuffer buffer(8);
    buffer.push(kPkgA, event_with_seq(1));
    buffer.mark_gap(kPkgA);
    buffer.push(kPkgA, event_with_seq(2));

    const PackageEventDrain drain = buffer.take(kPkgA);
    const bool daemon_gap_reported = drain.gapped;
    CHECK(daemon_gap_reported);
    CHECK(drain.dropped == 0);
    CHECK(buffer.dropped_total() == 0);
    // The positive artifact: the path DID produce events, so `dropped == 0` is a measured zero.
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({1, 2}));
}

// A drain CLEARS — including the loud pair. A `gapped` that latched forever would make every
// subsequent poll demand a re-snapshot, i.e. a permanently useless subscription.
void a_drain_clears_the_mailbox_and_the_loud_pair()
{
    PackageEventBuffer buffer(2);
    for (std::uint64_t seq = 1; seq <= 5; ++seq)
    {
        buffer.push(kPkgA, event_with_seq(seq));
    }
    const PackageEventDrain first = buffer.take(kPkgA);
    CHECK(first.gapped);
    CHECK(first.dropped == 3);

    const PackageEventDrain second = buffer.take(kPkgA);
    CHECK(second.events.empty());
    CHECK(second.dropped == 0);
    CHECK(!second.gapped);
    CHECK(buffer.pending(kPkgA) == 0);
    // The lifetime counter is NOT reset by a drain — it is the "has this editor ever dropped" fact.
    CHECK(buffer.dropped_total() == 3);
}

// THE BUDGET IS PER PACKAGE. One package flooding must not cost another package its events, or the
// bound would be a shared resource an untrusted package could exhaust on its neighbours' behalf.
void one_packages_flood_does_not_consume_another_packages_budget()
{
    PackageEventBuffer buffer(4);
    for (std::uint64_t seq = 1; seq <= 20; ++seq)
    {
        buffer.push(kPkgA, event_with_seq(seq));
    }
    buffer.push(kPkgB, event_with_seq(100));
    buffer.push(kPkgB, event_with_seq(101));

    const PackageEventDrain flooded = buffer.take(kPkgA);
    CHECK(flooded.gapped);
    CHECK(flooded.dropped == 16);

    const PackageEventDrain quiet = buffer.take(kPkgB);
    CHECK(seqs_of(quiet) == std::vector<std::uint64_t>({100, 101}));
    CHECK(quiet.dropped == 0);
    CHECK(!quiet.gapped);
}

// An unknown package drains EMPTY rather than refusing: editor-core polls unconditionally on its tick
// for every package it has a panel mounted for, so this is the ordinary case.
void an_unknown_package_drains_empty_rather_than_faulting()
{
    PackageEventBuffer buffer(4);
    const PackageEventDrain drain = buffer.take("never-subscribed");
    CHECK(drain.events.empty());
    CHECK(drain.dropped == 0);
    CHECK(!drain.gapped);
}

// A capacity of 0 would be "buffer nothing" — a disabled feature wearing a configuration error's
// clothes. Clamped to 1, exactly as PackageSessionHost clamps its own cap.
void a_zero_capacity_is_clamped_rather_than_disabling_delivery()
{
    PackageEventBuffer buffer(0);
    CHECK(buffer.capacity() == 1);
    buffer.push(kPkgA, event_with_seq(1));
    buffer.push(kPkgA, event_with_seq(2));
    const PackageEventDrain drain = buffer.take(kPkgA);
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({2}));
    CHECK(drain.dropped == 1);
    CHECK(drain.gapped);
}

// ------------------------------------------------- 2. the allowlist now carries the protocol

// e13c-1 held `subscribe` / `unsubscribe` / `ack` OUT because no bound existed. They are in NOW — and
// the write verbs are still out, which is the half that says the allowlist did not simply widen.
void the_subscription_protocol_is_panel_callable_and_the_write_verbs_are_still_not()
{
    CHECK(shell::is_panel_callable_daemon_method("subscribe"));
    CHECK(shell::is_panel_callable_daemon_method("unsubscribe"));
    CHECK(shell::is_panel_callable_daemon_method("ack"));
    // The e13c-1 entries are untouched.
    CHECK(shell::is_panel_callable_daemon_method("query"));
    // …and nothing that WRITES came along with them.
    CHECK(!shell::is_panel_callable_daemon_method("set"));
    CHECK(!shell::is_panel_callable_daemon_method("edit"));
    CHECK(!shell::is_panel_callable_daemon_method("build"));
    CHECK(!shell::is_panel_callable_daemon_method("package.add"));
    // EXACT match only — an allowlist that grew on a prefix rule would grant every future
    // `subscribe.<anything>` sight unseen (package_sessions.cpp § is_panel_callable_daemon_method).
    CHECK(!shell::is_panel_callable_daemon_method("subscribe.all"));
    CHECK(!shell::is_panel_callable_daemon_method("ack "));
    CHECK(shell::panel_callable_daemon_methods().size() == 7);
}

// ------------------------------------------------------------------ 3. the pump, over a real wire

// Every client the host mints, so a case can push server-originated frames onto a specific wire.
struct MintedWires
{
    std::vector<clientmock::MockChannel*> channels;
    // Every method that actually REACHED a wire, in order. Control 5's positive artifact: a refusal
    // that leaves this list unchanged proves the Shell answered BEFORE the daemon was asked, which is
    // the whole point of checking ownership ahead of the call.
    std::shared_ptr<std::vector<std::string>> wire_calls =
        std::make_shared<std::vector<std::string>>();
};

[[nodiscard]] PackageSessionHost::ClientFactory make_factory(MintedWires& wires)
{
    return [&wires](std::string&) -> std::unique_ptr<client::Client>
    {
        auto channel = std::make_unique<clientmock::MockChannel>();
        clientmock::MockChannel* raw = channel.get();
        raw->on("attach",
                [](const clientmock::Request&)
                {
                    // FLAT in `result`, as Dispatcher::handle really answers it. The protocol major
                    // is the CONSTANT, never a literal: a mock that hardcodes it keeps answering the
                    // old number after a bump and hides the reader bug the handshake exists to catch.
                    Json result = Json::object();
                    result.set("protocolMajor",
                               Json(static_cast<std::uint64_t>(contract::kProtocolMajor)));
                    result.set("clientId", Json(static_cast<std::uint64_t>(7)));
                    Json caps = Json::array();
                    caps.push_back(Json(std::string("describe")));
                    result.set("capabilities", std::move(caps));
                    Json scopes = Json::array();
                    scopes.push_back(Json(std::string("read-query")));
                    result.set("scopes", std::move(scopes));
                    return result;
                });
        // ONE COUNTER PER SESSION, so a package that subscribes N times receives N DISTINCT subIds —
        // which is what control 5's ledger keys on. A fixture answering `sub-1` every time would make
        // the ownership set a permanent singleton and the sub-cap unreachable, i.e. it would make both
        // control-5 cases pass without the control existing.
        auto next_sub = std::make_shared<std::uint64_t>(0);
        auto calls = wires.wire_calls;
        raw->on("subscribe",
                [next_sub, calls](const clientmock::Request&)
                {
                    calls->push_back("subscribe");
                    Json data = Json::object();
                    data.set("subId", Json("sub-" + std::to_string(++*next_sub)));
                    data.set("snapshot", clientmock::make_snapshot("inc-1", 0));
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        raw->on("unsubscribe",
                [calls](const clientmock::Request&)
                {
                    calls->push_back("unsubscribe");
                    Json data = Json::object();
                    data.set("removed", Json(true));
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        raw->on("ack",
                [calls](const clientmock::Request&)
                {
                    calls->push_back("ack");
                    Json data = Json::object();
                    data.set("acked", Json(true));
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        wires.channels.push_back(raw);
        return std::make_unique<client::Client>(std::move(channel));
    };
}

/** Open `package_id`'s session by making the one allowlisted call that opens a subscription. */
void subscribe_on(PackageSessionHost& host, const char* package_id)
{
    const shell::BridgeResult opened = host.forward(package_id, "subscribe", Json::object());
    CHECK(opened.error_code.empty());
}

// THE CHAIN THIS TASK EXISTS FOR, on the Shell side: a daemon PUSHES an event onto a package's own
// baseline session, the owner loop's pump moves it into that package's bounded buffer, and
// `panel.events.poll` hands it to editor-core. Driven through a REAL `client::Client`.
void the_pump_moves_pushed_daemon_events_into_the_calling_packages_buffer()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    // ⚠ TWO LIVE SESSIONS, PUMPED TOGETHER — the attribution cannot be tested with one. With a single
    // session every "landed in the right mailbox" claim is satisfied by an ABSENCE that holds because
    // the other package was never there: replacing the pump's `events_.push(session.package_id, …)`
    // with `events_.push(sessions_.front().package_id, …)` — i.e. every package's daemon events
    // landing in the FIRST package's mailbox, a cross-package disclosure on the control whose stated
    // purpose is per-package isolation — left this whole suite green. Two sessions with DISJOINT seq
    // ranges make both drains positive artifacts, so that mutation reddens.
    subscribe_on(host, kPkgA);
    subscribe_on(host, kPkgB);
    CHECK(wires.channels.size() == 2);

    // Nothing pushed yet: the pump is a no-op, and the poll is an ordinary empty drain.
    CHECK(host.pump() == 0);
    CHECK(host.poll_events(kPkgA).events.empty());

    wires.channels[0]->push_event("sub-1", event_with_seq(11));
    wires.channels[0]->push_event("sub-1", event_with_seq(12));
    wires.channels[1]->push_event("sub-1", event_with_seq(21));
    wires.channels[1]->push_event("sub-1", event_with_seq(22));
    CHECK(host.pump() == 4);
    CHECK(host.events_buffered() == 4);

    const PackageEventDrain drain = host.poll_events(kPkgA);
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({11, 12}));
    CHECK(!drain.gapped);
    CHECK(drain.dropped == 0);
    // The `subId` is carried BESIDE the envelope: the daemon puts it outside the event object, and a
    // package holding several subscriptions cannot demultiplex without it.
    CHECK(drain.events.at(0).contains("subId"));
    CHECK(drain.events.at(0).at("subId").as_string() == "sub-1");
    // AND THE OTHER PACKAGE GOT EXACTLY ITS OWN — the positive half. Note both wires carry the same
    // subId spelling on purpose: the mailbox is keyed by PACKAGE, so a demux that keyed on subId
    // would collapse these two and is caught here rather than by an absence.
    const PackageEventDrain other = host.poll_events(kPkgB);
    CHECK(seqs_of(other) == std::vector<std::uint64_t>({21, 22}));
    CHECK(!other.gapped);
    CHECK(other.dropped == 0);
    // Both mailboxes are now drained, so a package that never subscribed still reads empty.
    CHECK(host.poll_events("absent-panel").events.empty());
}

// The daemon's own `event.gap` frame reaches the panel as `gapped`, with `dropped` untouched.
void the_pump_relays_a_daemon_gap_without_inventing_a_drop()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    subscribe_on(host, kPkgA);

    wires.channels[0]->push_event("sub-1", event_with_seq(1));
    wires.channels[0]->push_gap();
    CHECK(host.pump() == 1); // the gap is not an EVENT — it is a flag

    const PackageEventDrain drain = host.poll_events(kPkgA);
    const bool pumped_gap_reported = drain.gapped;
    CHECK(pumped_gap_reported);
    CHECK(drain.dropped == 0);
    CHECK(host.events_dropped() == 0);
    CHECK(seqs_of(drain) == std::vector<std::uint64_t>({1}));
}

// THE RESPONSIVENESS BOUND, distinct from the memory bound: one pump takes at most
// `kMaxDrainedFramesPerPump` frames per session, and the remainder rides the next frame.
void one_pump_is_bounded_so_a_chatty_topic_cannot_hold_the_frame()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    subscribe_on(host, kPkgA);

    const std::size_t pushed = shell::kMaxDrainedFramesPerPump + 5;
    for (std::size_t i = 0; i < pushed; ++i)
    {
        wires.channels[0]->push_event("sub-1", event_with_seq(static_cast<std::uint64_t>(i + 1)));
    }
    CHECK(host.pump() == shell::kMaxDrainedFramesPerPump);
    CHECK(host.pump() == 5);
    CHECK(host.pump() == 0);
    CHECK(host.events_buffered() == pushed);
}

// The buffer dies with the sessions: an event's seq belongs to the incarnation of the connection that
// delivered it, so surviving a reset would hand a panel a cursor into a dead lifetime.
void a_reset_drops_the_buffered_events_with_the_sessions()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    subscribe_on(host, kPkgA);
    wires.channels[0]->push_event("sub-1", event_with_seq(1));
    CHECK(host.pump() == 1);

    host.reset();
    CHECK(host.sessions_open() == 0);
    const bool buffer_died_with_the_sessions = host.poll_events(kPkgA).events.empty();
    CHECK(buffer_died_with_the_sessions);
}

// ---------------------------------------------------------------- 4. served over a real router

void panel_events_poll_is_served_over_a_real_router()
{
    MintedWires wires;
    BridgeRouter router;
    PackageSessionHost host(make_factory(wires));
    CHECK(host.install(router));

    // A bad / missing `packageId` is a REFUSAL — the same predicate the fan-in validates against, so
    // a package cannot be one thing to the scheme and another to the buffer.
    const shell::BridgeDispatch bad = router.dispatch(
        R"({"jsonrpc":"2.0","id":1,"method":"panel.events.poll","params":{"packageId":"../evil"}})");
    const bool traversal_package_id_refused =
        shelltest::mentions(bad.response, shell::kErrPackageBadParams);
    CHECK(traversal_package_id_refused);
    const shell::BridgeDispatch missing = router.dispatch(
        R"({"jsonrpc":"2.0","id":2,"method":"panel.events.poll","params":{}})");
    CHECK(shelltest::mentions(missing.response, shell::kErrPackageBadParams));

    // A package with nothing buffered polls EMPTY and is NOT refused — the property that keeps the
    // live smokes' `bridge.refused() == 0` invariant true on every idle tick.
    const shell::BridgeDispatch idle = router.dispatch(
        R"({"jsonrpc":"2.0","id":3,"method":"panel.events.poll",)"
        R"("params":{"packageId":"hello-panel"}})");
    CHECK(!idle.refused());
    CHECK(shelltest::mentions(idle.response, "\"events\""));
    CHECK(shelltest::mentions(idle.response, "\"gapped\":false"));

    // Now the real thing: subscribe, push past the cap, pump, and read the LOUD pair off the WIRE
    // response — the only place a package can ever see it.
    subscribe_on(host, kPkgA);
    for (std::size_t i = 0; i < shell::kMaxBufferedEventsPerPackage + 3; ++i)
    {
        wires.channels[0]->push_event("sub-1", event_with_seq(static_cast<std::uint64_t>(i + 1)));
    }
    // More frames than one pump takes, so drive it until the wire is drained.
    while (host.pump() > 0)
    {
    }
    CHECK(host.events_dropped() == 3);

    const shell::BridgeDispatch drained = router.dispatch(
        R"({"jsonrpc":"2.0","id":4,"method":"panel.events.poll",)"
        R"("params":{"packageId":"hello-panel"}})");
    CHECK(!drained.refused());
    CHECK(shelltest::mentions(drained.response, "\"dropped\":3"));
    CHECK(shelltest::mentions(drained.response, "\"gapped\":true"));
    // The surviving head is seq 4 — the oldest three were the ones evicted.
    // TRAILING COMMA, deliberately: `mentions` is a plain substring find, so a bare `"seq":4` also
    // matches `"seq":40`, `"seq":45`, … which are present in the same reply — the assertion would
    // then hold for a head of almost anything. The comma pins the whole value.
    CHECK(shelltest::mentions(drained.response, "\"seq\":4,"));
    CHECK(!shelltest::mentions(drained.response, "\"seq\":1,"));

    // A second install is a WIRING BUG (the router refuses a duplicate name), and the caller checks.
    PackageSessionHost second(make_factory(wires));
    CHECK(!second.install(router));
}

} // namespace

// CONTROL 5 (S8) — a package may only address subscriptions it MINTED ITSELF.
//
// The hole this closes: `Dispatcher::dispatch` routes `unsubscribe`/`ack` to `serve_subscription`
// WITHOUT the connection's Session, and `EventStream::unsubscribe`/`::ack` then match on a
// daemon-global, sequential `sub-N` id that carries no owner. So without a Shell-side check a
// sandboxed package could walk the namespace and cancel the SHELL's own subscription, or advance a
// third party's ack cursor and prune ring history it never read.
void a_package_cannot_address_a_subscription_it_did_not_mint()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    subscribe_on(host, kPkgA);
    subscribe_on(host, kPkgB);
    CHECK(host.subscriptions_open(kPkgA) == 1);
    CHECK(host.subscriptions_open(kPkgB) == 1);
    const std::size_t reached_the_wire = wires.wire_calls->size();

    // An id this package did not mint — the shape a walk of the sequential namespace produces.
    Json foreign = Json::object();
    foreign.set("subId", Json(std::string("sub-99")));
    CHECK(host.forward(kPkgA, "unsubscribe", foreign).error_code == shell::kErrPackageUnknownSubscription);
    CHECK(host.forward(kPkgA, "ack", foreign).error_code == shell::kErrPackageUnknownSubscription);
    CHECK(host.refused_subscriptions() == 2);
    // THE POSITIVE ARTIFACT, and the reason the check sits ahead of `session_for`: neither call
    // reached a wire at all, so the daemon was never given the chance to act on someone else's id —
    // and its "unknown vs live" answer never became an existence oracle.
    CHECK(wires.wire_calls->size() == reached_the_wire);

    // …while the package's OWN id still travels, so the control refuses the right thing rather than
    // everything. Note kPkgB's subscription is spelled `sub-1` TOO (its own session, its own counter):
    // if ownership were keyed on the id alone rather than per package, A's unsubscribe would cancel
    // B's subscription here.
    Json own = Json::object();
    own.set("subId", Json(std::string("sub-1")));
    CHECK(host.forward(kPkgA, "unsubscribe", own).error_code.empty());
    CHECK(wires.wire_calls->size() == reached_the_wire + 1);
    CHECK(host.subscriptions_open(kPkgA) == 0);
    CHECK(host.subscriptions_open(kPkgB) == 1);

    // A package with no session at all is refused identically, and opens no connection doing it.
    const std::size_t sessions_before = wires.channels.size();
    CHECK(host.forward("third-panel", "ack", own).error_code == shell::kErrPackageUnknownSubscription);
    CHECK(wires.channels.size() == sessions_before);
}

// The second half of control 5: a package cannot mint subIds without bound. Each one costs the DAEMON
// a Subscriber with its own queue and one more fan-out of every matching event, in the process that
// also serves the CLI and every AI client — a cost this editor's 256-event buffer does not bound.
void the_subscription_sub_cap_bounds_what_one_package_can_mint()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    for (std::size_t i = 0; i < shell::kMaxSubscriptionsPerPackage; ++i)
    {
        subscribe_on(host, kPkgA);
    }
    CHECK(host.subscriptions_open(kPkgA) == shell::kMaxSubscriptionsPerPackage);

    const std::size_t reached_the_wire = wires.wire_calls->size();
    CHECK(host.forward(kPkgA, "subscribe", Json::object()).error_code ==
          shell::kErrPackageSubscriptionCapacity);
    // Refused BEFORE the daemon minted anything — an over-cap subscribe that reached the wire would
    // leave a live subscription this Shell has no record of and can therefore never release.
    CHECK(wires.wire_calls->size() == reached_the_wire);

    // PER PACKAGE, like the buffer's own cap: a different package is entirely unaffected.
    subscribe_on(host, kPkgB);
    CHECK(host.subscriptions_open(kPkgB) == 1);

    // And `unsubscribe` is a real release valve rather than a second way to spend the budget.
    Json own = Json::object();
    own.set("subId", Json(std::string("sub-1")));
    CHECK(host.forward(kPkgA, "unsubscribe", own).error_code.empty());
    CHECK(host.subscriptions_open(kPkgA) == shell::kMaxSubscriptionsPerPackage - 1);
    CHECK(host.forward(kPkgA, "subscribe", Json::object()).error_code.empty());
    CHECK(host.subscriptions_open(kPkgA) == shell::kMaxSubscriptionsPerPackage);
}

int main()
{
    under_the_cap_every_event_is_delivered_in_order_and_nothing_is_reported_lost();
    the_cap_is_real_and_the_oldest_events_are_the_ones_dropped();
    a_daemon_gap_sets_gapped_without_claiming_this_editor_dropped_anything();
    a_drain_clears_the_mailbox_and_the_loud_pair();
    one_packages_flood_does_not_consume_another_packages_budget();
    an_unknown_package_drains_empty_rather_than_faulting();
    a_zero_capacity_is_clamped_rather_than_disabling_delivery();
    the_subscription_protocol_is_panel_callable_and_the_write_verbs_are_still_not();
    the_pump_moves_pushed_daemon_events_into_the_calling_packages_buffer();
    the_pump_relays_a_daemon_gap_without_inventing_a_drop();
    one_pump_is_bounded_so_a_chatty_topic_cannot_hold_the_frame();
    a_reset_drops_the_buffered_events_with_the_sessions();
    panel_events_poll_is_served_over_a_real_router();
    a_package_cannot_address_a_subscription_it_did_not_mint();
    the_subscription_sub_cap_bounds_what_one_package_can_mint();
    SHELL_TEST_MAIN_END();
}
