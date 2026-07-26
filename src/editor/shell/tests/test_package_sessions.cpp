// T1 for the per-package BASELINE daemon sessions + the `panel.daemon.call` fan-in route
// (M9 e13c-1, design 04 §5 / 08 §2).
//
// WHAT THIS PROVES AND WHY EACH HALF MATTERS. This route is the first path by which THIRD-PARTY code
// reaches the daemon at all, so the properties pinned here are the ones whose failure is a capability
// escalation rather than a bug:
//
//   * THE SCOPE IS THE BASELINE, PROVED AGAINST THE REAL DISPATCHER. `scope_baseline_is_refused_*`
//     does not assert a string — it feeds `kPackageSessionScope` through the REAL
//     `bridge::ScopeSet::parse` -> `Dispatcher::attach` -> `Dispatcher::dispatch` chain and asserts
//     the daemon's own `scope.denied` comes back for `set` and does NOT for `query`. That is what
//     makes the required non-vacuity plant meaningful: widen the constant and the refusal disappears.
//     A test that merely compared the constant to `"read"` would pass the plant `"read"` -> `"read "`
//     and fail the plant `"read"` -> `"read,write"` for the wrong reason (a spelling, not a grant).
//
//   * THE ALLOWLIST IS THE FORWARDER'S, NOT THE SCOPE TABLE'S (S4/S7). `required_scope_for` defaults
//     an unknown method to `read_query`, so a panel-chosen method the dispatcher does not classify
//     would run at the baseline. The suite therefore asserts a non-allowlisted method is refused
//     BEFORE any session is opened — the `sessions_open() == 0` half is the load-bearing one, because
//     a refusal that still attached would have proved only that the ANSWER was refused, not that the
//     control ran first.
//
//   * THE PACKAGE IDENTITY CANNOT BE FORGED INTO A WIDER SESSION. Every session is minted by the SAME
//     factory with the SAME hardcoded scope, so `a_second_package_gets_its_own_session` pins that two
//     packages are two sessions (attributable in the daemon's `clients` topic) while neither can be
//     more privileged than the other.
//
// THE MOCK IS AT THE WIRE (mock_channel.h's standing warning), so a REAL `client::Client` parses real
// dispatcher-shaped frames — including the FLAT attach reply, whose enveloping by a too-forgiving mock
// once hid an empty `granted_scopes()` for six tasks. That is also what lets the suite read the scope
// this class REQUESTED off the recorded `attach` params rather than trusting its own constant twice.

#include "context/editor/shell/package_sessions.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/shell/ipc_bridge.h"

#include "mock_channel.h"
#include "shell_test.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace bridge = context::editor::bridge;
namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace shell = context::editor::shell;

using contract::Json;
using shell::BridgeRequest;
using shell::BridgeResult;
using shell::BridgeRouter;
using shell::PackageSessionHost;

// ------------------------------------------------------------------------------------- the fixture

// Every client the host mints, in creation order, so a test can read what was sent on each wire.
struct MintedWires
{
    std::vector<clientmock::MockChannel*> channels;
    std::size_t minted = 0;
    // When set, the factory refuses — the "no daemon" state a welcome-screen boot is genuinely in.
    bool refuse = false;
};

// A factory that mints UNATTACHED clients over scripted wires. It scripts ONLY `attach` (flat, as the
// dispatcher really answers it) and `query`; anything else is answered by MockChannel's permissive
// default, which is exactly the posture that would let an allowlist bug pass unnoticed — so the
// allowlist assertions below key on `sessions_open()` and the refusal CODE, never on the wire's
// silence.
[[nodiscard]] PackageSessionHost::ClientFactory make_factory(MintedWires& wires)
{
    return [&wires](std::string& error) -> std::unique_ptr<client::Client>
    {
        if (wires.refuse)
        {
            error = "no discoverable daemon";
            return nullptr;
        }
        auto channel = std::make_unique<clientmock::MockChannel>();
        clientmock::MockChannel* raw = channel.get();
        raw->on("attach",
                [](const clientmock::Request&)
                {
                    // FLAT in `result`, NOT an envelope — the shape Dispatcher::handle really emits.
                    Json result = Json::object();
                    result.set("protocolMajor",
                               Json(static_cast<std::uint64_t>(contract::kProtocolMajor)));
                    result.set("clientId", Json(static_cast<std::uint64_t>(7)));
                    Json caps = Json::array();
                    caps.push_back(Json(std::string("describe")));
                    result.set("capabilities", std::move(caps));
                    Json scopes = Json::array();
                    // The daemon reports back the CLAMPED set. A baseline session gets read-query and
                    // nothing else — modelled here so a widened request would be visibly at odds with
                    // what the daemon granted.
                    scopes.push_back(Json(std::string("read-query")));
                    result.set("scopes", std::move(scopes));
                    return result;
                });
        raw->on("query",
                [](const clientmock::Request&)
                {
                    Json data = Json::object();
                    data.set("rows", Json::array());
                    return clientmock::MockChannel::ok_envelope(std::move(data));
                });
        wires.channels.push_back(raw);
        ++wires.minted;
        return std::make_unique<client::Client>(std::move(channel));
    };
}

// ------------------------------------------------------------------- 1. the baseline scope, for real

// THE PLANT TARGET. `kPackageSessionScope` is fed through the REAL parse + attach + dispatch chain,
// so widening it makes the refusal below disappear — which is precisely the required non-vacuity
// demonstration, and precisely what a string comparison could not show.
void the_baseline_scope_is_refused_a_file_write_by_the_real_dispatcher()
{
    const bridge::Dispatcher dispatcher; // ceiling = all scopes, i.e. NOTHING is clamping but us
    contract::ClientHandshake handshake;
    handshake.protocol_major = contract::kProtocolMajor;

    const bridge::Dispatcher::AttachResult attached =
        dispatcher.attach(handshake, bridge::ScopeSet::parse(shell::kPackageSessionScope));
    const bridge::Session* session = std::get_if<bridge::Session>(&attached);
    CHECK(session != nullptr);
    if (session == nullptr)
    {
        return;
    }

    // THE DoD LINE, observable: an un-granted authored-file write is refused IN THE DISPATCHER, with
    // the catalog's own code, on the session a package panel would be riding.
    const contract::Envelope wrote = dispatcher.dispatch("set", Json::object(), *session);
    CHECK(!wrote.ok());
    CHECK(wrote.error().has_value());
    CHECK(wrote.error().has_value() && wrote.error()->code == bridge::kScopeDeniedCode);

    // …and its `build_install` sibling, the second half of the same DoD box.
    const contract::Envelope built = dispatcher.dispatch("build", Json::object(), *session);
    CHECK(built.error().has_value() && built.error()->code == bridge::kScopeDeniedCode);
    const contract::Envelope installed = dispatcher.dispatch("package.add", Json::object(), *session);
    CHECK(installed.error().has_value() && installed.error()->code == bridge::kScopeDeniedCode);

    // THE NON-VACUITY CONTROL. Without this the three refusals above would pass just as happily
    // against a session that could do NOTHING — including a bug that refused every method — so the
    // suite would be asserting "the daemon is broken" and calling it security.
    const contract::Envelope read = dispatcher.dispatch("query", Json::object(), *session);
    CHECK(!(read.error().has_value() && read.error()->code == bridge::kScopeDeniedCode));

    // And the clamp is real in the other direction: a session that DID hold file_write is not
    // scope-denied `set`. This is what the plant flips, so it is pinned as the inverse rather than
    // left implicit.
    const bridge::Dispatcher::AttachResult widened =
        dispatcher.attach(handshake, bridge::ScopeSet::parse("read,write,build"));
    const bridge::Session* wide = std::get_if<bridge::Session>(&widened);
    CHECK(wide != nullptr);
    if (wide != nullptr)
    {
        const contract::Envelope allowed = dispatcher.dispatch("set", Json::object(), *wide);
        CHECK(!(allowed.error().has_value() &&
                allowed.error()->code == bridge::kScopeDeniedCode));
    }
}

// The scope this class REQUESTS is read off the wire, not off its own constant — the half that proves
// `kPackageSessionScope` actually reaches `AttachOptions`, which the dispatcher test above assumes.
void the_requested_scope_on_the_wire_is_the_baseline()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));

    const BridgeResult served = host.forward("hello-panel", "query", Json::object());
    CHECK(served.error_code.empty());
    CHECK(wires.minted == 1);
    CHECK(wires.channels.size() == 1);
    if (wires.channels.empty())
    {
        return;
    }
    const std::vector<clientmock::Request> attaches = wires.channels[0]->requests_for("attach");
    CHECK(attaches.size() == 1);
    if (attaches.empty())
    {
        return;
    }
    CHECK(attaches[0].params.at("scope").is_string());
    CHECK(attaches[0].params.at("scope").as_string() == std::string(shell::kPackageSessionScope));
    CHECK(attaches[0].params.at("scope").as_string() == "read");
    // NO TOKEN IS THREADED THROUGH THIS CLASS: `attach()` falls back to the token
    // `connect_to_project` discovered, so the request this class built carries none of its own.
    CHECK(!attaches[0].params.contains("token") || attaches[0].params.at("token").as_string().empty());
}

// ------------------------------------------------------------------------------- 2. the allowlist

void the_allowlist_is_closed_and_small()
{
    // Named members, so a future task that adds one must come here and say so.
    CHECK(shell::is_panel_callable_daemon_method("describe"));
    CHECK(shell::is_panel_callable_daemon_method("query"));
    CHECK(shell::is_panel_callable_daemon_method("editor.scene-tree"));
    CHECK(shell::is_panel_callable_daemon_method("editor.inspect"));
    CHECK(shell::panel_callable_daemon_methods().size() == 4);

    // The write / build families — refused HERE as well as by the dispatcher. Both controls,
    // independently: that redundancy IS S4.
    CHECK(!shell::is_panel_callable_daemon_method("set"));
    CHECK(!shell::is_panel_callable_daemon_method("edit"));
    CHECK(!shell::is_panel_callable_daemon_method("build"));
    CHECK(!shell::is_panel_callable_daemon_method("package.add"));
    // The e13c-2 surface, held out on purpose (an unbounded fan-out buffer is the reason).
    CHECK(!shell::is_panel_callable_daemon_method("subscribe"));
    CHECK(!shell::is_panel_callable_daemon_method("ack"));
    // S7: a method the scope table does NOT classify defaults to `read_query`, so the allowlist is
    // the ONLY thing standing between a panel and an unrecognised-but-backend-served verb. Pinned as
    // a pair so the two facts cannot drift apart.
    CHECK(bridge::required_scope_for("totally.unknown.verb") == bridge::Scope::read_query);
    CHECK(!shell::is_panel_callable_daemon_method("totally.unknown.verb"));

    // NOT a prefix rule (unlike the forbidden-method DENYLIST): an allowlist that widened on its own
    // would grant every future `query.<x>` sight unseen.
    CHECK(!shell::is_panel_callable_daemon_method("query.raw"));
    CHECK(!shell::is_panel_callable_daemon_method("describe.all"));
    CHECK(!shell::is_panel_callable_daemon_method(""));
    // …and not a SUBSTRING rule either, in the other direction.
    CHECK(!shell::is_panel_callable_daemon_method("re-query"));
}

// THE PLANTED NEGATIVE the DoD asks for: a non-allowlisted method is refused, and refused BEFORE a
// connection is spent on it.
void a_non_allowlisted_method_is_refused_without_opening_a_session()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));

    const BridgeResult refused = host.forward("hello-panel", "set", Json::object());
    CHECK(refused.error_code == shell::kErrPackageMethodNotAllowed);
    CHECK(host.refused_methods() == 1);
    CHECK(host.calls_forwarded() == 0);
    // THE LOAD-BEARING HALF. A refusal that had still attached would prove only that the ANSWER was
    // refused — the control has to run ahead of the connection, or probing for un-allowlisted methods
    // is itself a connection-exhaustion primitive.
    CHECK(host.sessions_open() == 0);
    CHECK(wires.minted == 0);

    // The refusal is IDENTICAL for a method that does not exist at all, so it is no oracle for the
    // daemon's verb surface.
    const BridgeResult unknown = host.forward("hello-panel", "no.such.verb", Json::object());
    CHECK(unknown.error_code == shell::kErrPackageMethodNotAllowed);
    CHECK(host.refused_methods() == 2);
    CHECK(host.sessions_open() == 0);
}

// ---------------------------------------------------------------------- 3. sessions: lazy, pooled

void a_mounted_package_that_never_calls_costs_no_connection()
{
    MintedWires wires;
    const PackageSessionHost host(make_factory(wires));
    // Construction alone must not attach — control 4(a). The whole point of lazy attach is that N
    // mounted packages are not N connections.
    CHECK(host.sessions_open() == 0);
    CHECK(wires.minted == 0);
}

void one_package_holds_exactly_one_pooled_session()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));

    CHECK(host.forward("hello-panel", "query", Json::object()).error_code.empty());
    CHECK(host.forward("hello-panel", "describe", Json::object()).error_code.empty());
    CHECK(host.forward("hello-panel", "query", Json::object()).error_code.empty());

    CHECK(host.calls_forwarded() == 3);
    // ONE connection for three calls — and, since the router is shared by every window, one for a
    // package with panels in three windows too.
    CHECK(host.sessions_open() == 1);
    CHECK(wires.minted == 1);
    CHECK(host.has_session("hello-panel"));
}

void a_second_package_gets_its_own_session()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));

    CHECK(host.forward("hello-panel", "query", Json::object()).error_code.empty());
    CHECK(host.forward("other-pkg", "query", Json::object()).error_code.empty());

    CHECK(host.sessions_open() == 2);
    CHECK(host.has_session("hello-panel"));
    CHECK(host.has_session("other-pkg"));
    // Each package attaches on its OWN wire, so the daemon's `clients` topic can attribute a call to
    // a package — the property pooling by scope-set instead would have lost.
    CHECK(wires.channels.size() == 2);
    if (wires.channels.size() == 2)
    {
        CHECK(wires.channels[0]->requests_for("attach").size() == 1);
        CHECK(wires.channels[1]->requests_for("attach").size() == 1);
        // …and both at the SAME baseline: two sessions, neither more privileged than the other.
        CHECK(wires.channels[1]->requests_for("attach")[0].params.at("scope").as_string() == "read");
    }
}

void the_sub_cap_bounds_connection_exhaustion()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires), /*max_sessions*/ 2);

    CHECK(host.forward("pkg-a", "query", Json::object()).error_code.empty());
    CHECK(host.forward("pkg-b", "query", Json::object()).error_code.empty());
    CHECK(host.sessions_open() == 2);

    const BridgeResult over = host.forward("pkg-c", "query", Json::object());
    CHECK(over.error_code == shell::kErrPackageCapacity);
    CHECK(host.refused_capacity() == 1);
    // Refused HERE, so the daemon never saw a connect it would have answered `daemon.busy`.
    CHECK(host.sessions_open() == 2);
    CHECK(wires.minted == 2);

    // An ALREADY-POOLED package is unaffected by the cap — the bound is on distinct sessions, not on
    // calls, so a full table does not stop the packages that are already talking.
    CHECK(host.forward("pkg-a", "query", Json::object()).error_code.empty());
    CHECK(host.sessions_open() == 2);

    // 0 clamps to 1 rather than disabling the feature (mirrors set_max_connections' own handling).
    MintedWires zero_wires;
    PackageSessionHost zero(make_factory(zero_wires), /*max_sessions*/ 0);
    CHECK(zero.forward("pkg-a", "query", Json::object()).error_code.empty());
    CHECK(zero.forward("pkg-b", "query", Json::object()).error_code == shell::kErrPackageCapacity);

    host.reset();
    CHECK(host.sessions_open() == 0);
}

// ------------------------------------------------------------------------------ 4. honest refusals

void a_shell_with_no_daemon_refuses_honestly()
{
    MintedWires wires;
    wires.refuse = true;
    PackageSessionHost host(make_factory(wires));

    const BridgeResult refused = host.forward("hello-panel", "query", Json::object());
    CHECK(refused.error_code == shell::kErrPackageNoSession);
    CHECK(host.sessions_open() == 0);
    // The factory's own diagnostic is Shell state and must NOT reach untrusted panel code (it would
    // carry the project path); the message names the package instead.
    CHECK(!shelltest::mentions(refused.error_message, "no discoverable daemon"));
    CHECK(shelltest::mentions(refused.error_message, "hello-panel"));
}

void a_daemon_refusal_reaches_the_panel_with_its_own_catalog_code()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));
    // Open the session first so the refusal below is the DAEMON's, not this class's.
    CHECK(host.forward("hello-panel", "query", Json::object()).error_code.empty());
    CHECK(wires.channels.size() == 1);
    if (wires.channels.empty())
    {
        return;
    }
    // The refusal a widened request would really meet, in the shape dispatcher.cpp emits it.
    wires.channels[0]->fail_method("query", "The attach token's scope does not permit 'query'.",
                                   bridge::kScopeDeniedCode);

    const BridgeResult denied = host.forward("hello-panel", "query", Json::object());
    // VERBATIM — re-classifying it here would hide WHICH control fired, and `scope.denied` is the one
    // fact this whole task exists to make reachable from a panel.
    CHECK(denied.error_code == bridge::kScopeDeniedCode);
}

void a_malformed_package_id_is_refused()
{
    MintedWires wires;
    PackageSessionHost host(make_factory(wires));

    // The SAME predicate the asset scheme mounts against, so a package cannot be one thing to the
    // scheme and another to the session table.
    CHECK(host.forward("", "query", Json::object()).error_code == shell::kErrPackageBadParams);
    CHECK(host.forward("../escape", "query", Json::object()).error_code ==
          shell::kErrPackageBadParams);
    CHECK(host.forward("UPPER", "query", Json::object()).error_code == shell::kErrPackageBadParams);
    CHECK(host.forward("hello-panel", "", Json::object()).error_code == shell::kErrPackageBadParams);
    CHECK(host.sessions_open() == 0);
    CHECK(wires.minted == 0);
}

// ------------------------------------------------------------------------------ 5. over the router

void served_over_a_real_router()
{
    MintedWires wires;
    BridgeRouter router;
    PackageSessionHost host(make_factory(wires));
    CHECK(host.install(router));

    // A second install is a WIRING BUG (the router refuses a duplicate name), and the caller checks.
    PackageSessionHost second(make_factory(wires));
    CHECK(!second.install(router));

    const shell::BridgeDispatch ok = router.dispatch(
        R"({"jsonrpc":"2.0","id":1,"method":"panel.daemon.call",)"
        R"("params":{"packageId":"hello-panel","method":"query"}})");
    CHECK(!ok.refused());
    CHECK(host.calls_forwarded() == 1);

    // A missing `params` member is the ordinary NO-ARGUMENT call, not a malformed one.
    const shell::BridgeDispatch bare = router.dispatch(
        R"({"jsonrpc":"2.0","id":2,"method":"panel.daemon.call",)"
        R"("params":{"packageId":"hello-panel","method":"describe"}})");
    CHECK(!bare.refused());
    CHECK(host.calls_forwarded() == 2);

    // A non-string `method` is refused at the envelope read, before anything else.
    const shell::BridgeDispatch bad = router.dispatch(
        R"({"jsonrpc":"2.0","id":3,"method":"panel.daemon.call",)"
        R"("params":{"packageId":"hello-panel","method":42}})");
    CHECK(shelltest::mentions(bad.response, shell::kErrPackageBadParams));

    // AND THE ROUTE IS NOT A HOLE IN CONTROL 2 OF ipc_bridge.h: forwarding is confined to the
    // allowlist, so the credential-bearing names the router refuses BY NAME cannot be smuggled
    // through as a payload — the exact bypass a denylist over an open namespace would have.
    const shell::BridgeDispatch smuggled = router.dispatch(
        R"({"jsonrpc":"2.0","id":4,"method":"panel.daemon.call",)"
        R"("params":{"packageId":"hello-panel","method":"instance.read"}})");
    CHECK(shelltest::mentions(smuggled.response, shell::kErrPackageMethodNotAllowed));
    CHECK(host.calls_forwarded() == 2);
    for (const std::string& forbidden : shell::forbidden_bridge_methods())
    {
        CHECK(!shell::is_panel_callable_daemon_method(forbidden));
    }
}

} // namespace

int main()
{
    the_baseline_scope_is_refused_a_file_write_by_the_real_dispatcher();
    the_requested_scope_on_the_wire_is_the_baseline();
    the_allowlist_is_closed_and_small();
    a_non_allowlisted_method_is_refused_without_opening_a_session();
    a_mounted_package_that_never_calls_costs_no_connection();
    one_package_holds_exactly_one_pooled_session();
    a_second_package_gets_its_own_session();
    the_sub_cap_bounds_connection_exhaustion();
    a_shell_with_no_daemon_refuses_honestly();
    a_daemon_refusal_reaches_the_panel_with_its_own_catalog_code();
    a_malformed_package_id_is_refused();
    served_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
