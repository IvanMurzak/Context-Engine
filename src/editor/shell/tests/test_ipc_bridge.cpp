// ctest `editor-shell-test_ipc_bridge` — the privileged native<->JS bridge (M9 e05c, design 04 §1
// / 08 §1-§2).
//
// This is the T1 adversarial suite the DoD names ("malformed/hostile inbound bridge messages
// rejected without crashing the Shell") plus the executable proof of the three token-isolation
// controls. Every message here is treated as what it really is on this channel: untrusted input
// from renderer-process content.
//
// It is CEF-free by design (ipc_bridge.h explains why), so it runs on all three default `build`
// legs rather than only on the per-OS CEF job.

#include "context/editor/shell/ipc_bridge.h"

#include "context/editor/shell/app_scheme.h"
#include "shell_test.h"

#include <stdexcept>
#include <string>

namespace shell = context::editor::shell;
namespace contract = context::editor::contract;

namespace
{

// A stand-in for the real D20 attach token: long enough to be scanned for, and distinctive enough
// that a match in a response is unambiguous.
const char* kFakeToken = "b6f2c1a48d3e07995cafe1234567890abcdef0123456789";
const char* kFakeEndpoint = "\\\\.\\pipe\\context-daemon-7f3a9c21";

// Configures IN PLACE rather than returning a router: BridgeRouter is deliberately non-movable
// (the CEF handler holds a raw pointer to the Shell's one router), so a factory returning by value
// would not compile — which is the guard working as designed.
void configure_router(shell::BridgeRouter& router)
{
    CHECK(router.register_method("shell.ping", [](const shell::BridgeRequest& request) {
        contract::Json value = contract::Json::object();
        value.set("pong", contract::Json(true));
        value.set("echo", request.params.at("text"));
        return shell::BridgeResult::ok(value);
    }));
    CHECK(router.register_method("shell.fail", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::error("shell.refused", "deliberate handler error");
    }));
    CHECK(router.register_method(
        "shell.throw", [](const shell::BridgeRequest&) -> shell::BridgeResult {
            throw std::runtime_error("a handler that does the worst thing");
        }));
}

std::string request(const char* method, const char* params_json = "{}", const char* id = "1")
{
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id + ",\"method\":\"" + method +
           "\",\"params\":" + params_json + "}";
}

// ---------------------------------------------------------------------------- the happy path

void test_round_trip()
{
    shell::BridgeRouter router;
    configure_router(router);

    const shell::BridgeDispatch out =
        router.dispatch(request("shell.ping", "{\"text\":\"hello\"}", "42"));
    CHECK(!out.refused());
    CHECK(router.served() == 1);
    CHECK(router.refused() == 0);

    // The envelope MIRRORS THE DAEMON'S (JSON-RPC 2.0) so e05d's client layers on cleanly.
    const contract::Json response = contract::Json::parse(out.response);
    CHECK(response.is_object());
    CHECK(response.at("jsonrpc").as_string() == "2.0");
    CHECK(response.at("id").as_int() == 42);
    CHECK(response.at("result").at("pong").as_bool());
    CHECK(response.at("result").at("echo").as_string() == "hello");
    CHECK(!response.contains("error"));

    // Absent params is legal and arrives as an empty object, not null.
    const shell::BridgeDispatch bare =
        router.dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shell.ping\"}");
    CHECK(!bare.refused());
}

void test_handler_error_is_not_a_refusal()
{
    shell::BridgeRouter router;
    configure_router(router);
    const shell::BridgeDispatch out = router.dispatch(request("shell.fail"));
    // The ENVELOPE was valid, so this is not a refusal — the handler simply said no.
    CHECK(!out.refused());
    CHECK(router.served() == 1);

    const contract::Json response = contract::Json::parse(out.response);
    CHECK(response.contains("error"));
    CHECK(!response.contains("result"));
    CHECK(response.at("id").as_int() == 1);
    CHECK(response.at("error").at("data").at("reason").as_string() == "shell.refused");
}

// -------------------------------------------------------------- T1: malformed / hostile input

void test_malformed_input_is_refused_not_fatal()
{
    shell::BridgeRouter router;
    configure_router(router);

    struct Case
    {
        const char* message;
        shell::BridgeReject expected;
    };

    const Case cases[] = {
        // Not JSON at all.
        {"", shell::BridgeReject::not_json},
        {"not json", shell::BridgeReject::not_json},
        {"{", shell::BridgeReject::not_json},
        {"{\"jsonrpc\":", shell::BridgeReject::not_json},
        {"\xff\xfe\x00\x01", shell::BridgeReject::not_json},
        // Valid JSON, wrong shape.
        {"[]", shell::BridgeReject::not_object},
        {"null", shell::BridgeReject::not_object},
        {"\"a string\"", shell::BridgeReject::not_object},
        {"123", shell::BridgeReject::not_object},
        {"[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\"}]",
         shell::BridgeReject::not_object},
        // The id.
        {"{\"jsonrpc\":\"2.0\",\"method\":\"shell.ping\"}", shell::BridgeReject::bad_id},
        {"{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"shell.ping\"}",
         shell::BridgeReject::bad_id},
        {"{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"shell.ping\"}",
         shell::BridgeReject::bad_id},
        {"{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"shell.ping\"}", shell::BridgeReject::bad_id},
        // Beyond JS's safe-integer range: it could not round-trip through the renderer.
        {"{\"jsonrpc\":\"2.0\",\"id\":1e300,\"method\":\"shell.ping\"}",
         shell::BridgeReject::bad_id},
        {"{\"jsonrpc\":\"2.0\",\"id\":-1e300,\"method\":\"shell.ping\"}",
         shell::BridgeReject::bad_id},
        // The version tag.
        {"{\"id\":1,\"method\":\"shell.ping\"}", shell::BridgeReject::bad_jsonrpc},
        {"{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"shell.ping\"}",
         shell::BridgeReject::bad_jsonrpc},
        {"{\"jsonrpc\":2.0,\"id\":1,\"method\":\"shell.ping\"}", shell::BridgeReject::bad_jsonrpc},
        // The method.
        {"{\"jsonrpc\":\"2.0\",\"id\":1}", shell::BridgeReject::bad_method},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\"}", shell::BridgeReject::bad_method},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":42}", shell::BridgeReject::bad_method},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":[\"shell.ping\"]}",
         shell::BridgeReject::bad_method},
        // params must be an object when present — an array or scalar would push the shape question
        // into every handler, which is where it gets forgotten.
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\",\"params\":[]}",
         shell::BridgeReject::bad_params},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\",\"params\":\"x\"}",
         shell::BridgeReject::bad_params},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\",\"params\":7}",
         shell::BridgeReject::bad_params},
        // Deny-by-default routing.
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.nope\"}",
         shell::BridgeReject::unknown_method},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eval\"}", shell::BridgeReject::unknown_method},
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"../../etc/passwd\"}",
         shell::BridgeReject::unknown_method},
        // A handler that throws is CONTAINED — an exception unwinding into CEF's message-router
        // callback would take the whole Shell down. This is the DoD's "without crashing the Shell".
        {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.throw\"}",
         shell::BridgeReject::handler_threw},
    };

    std::size_t refusals = 0;
    for (const Case& c : cases)
    {
        const shell::BridgeDispatch out = router.dispatch(c.message);
        CHECK(out.refused());
        CHECK(out.reject == c.expected);
        ++refusals;

        // EVERY refusal is still a well-formed JSON-RPC error envelope: a renderer that sent
        // garbage gets a legible answer instead of a hang.
        const contract::Json response = contract::Json::parse(out.response);
        CHECK(response.is_object());
        CHECK(response.at("jsonrpc").as_string() == "2.0");
        CHECK(response.contains("error"));
        CHECK(!response.contains("result"));
        CHECK(response.at("error").at("data").at("reason").as_string() ==
              std::string(shell::to_string(c.expected)));
    }

    CHECK(router.refused() == refusals);
    CHECK(router.served() == 0);

    // Still alive and still correct after every hostile message above — the property the DoD line
    // actually asserts.
    const shell::BridgeDispatch after =
        router.dispatch(request("shell.ping", "{\"text\":\"still here\"}"));
    CHECK(!after.refused());
}

void test_size_cap()
{
    shell::BridgeRouter router;
    configure_router(router);
    // A renderer can send an arbitrarily large string; parsing one would be an unbounded allocation
    // driven by untrusted input.
    std::string huge = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\",\"params\":{\"t\":\"";
    huge.append(shell::kMaxBridgeMessageBytes + 16, 'A');
    huge += "\"}}";
    const shell::BridgeDispatch out = router.dispatch(huge);
    CHECK(out.refused());
    CHECK(out.reject == shell::BridgeReject::too_large);

    // Just under the cap is parsed normally (so the cap is a cap, not a coincidence).
    CHECK(router.dispatch(request("shell.ping", "{\"text\":\"small\"}")).refused() == false);
}

void test_deep_nesting_is_contained()
{
    shell::BridgeRouter router;
    configure_router(router);
    // A deeply nested payload is the classic parser-recursion attack. Whatever the parser does with
    // it — accept or throw — the bridge must not propagate an exception to its caller.
    std::string nested = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"shell.ping\",\"params\":{\"a\":";
    const int depth = 2000;
    for (int i = 0; i < depth; ++i)
    {
        nested += "[";
    }
    for (int i = 0; i < depth; ++i)
    {
        nested += "]";
    }
    nested += "}}";
    const shell::BridgeDispatch out = router.dispatch(nested);
    // Either outcome is acceptable; NOT crashing is the assertion.
    CHECK(out.refused() || !out.response.empty());
    CHECK(!router.dispatch(request("shell.ping", "{\"text\":\"alive\"}")).refused());
}

// ------------------------------------------------- the three token-isolation controls (08 §1)

void test_control_2_forbidden_methods_cannot_be_registered()
{
    shell::BridgeRouter router;
    CHECK(!shell::forbidden_bridge_methods().empty());

    // CONTROL 2, the structural half: a maintainer who wires a credential-bearing method through
    // the bridge does not get a working leak plus a review comment — they get `false` back.
    for (const std::string& method : shell::forbidden_bridge_methods())
    {
        const bool registered = router.register_method(method, [](const shell::BridgeRequest&) {
            return shell::BridgeResult::ok(contract::Json("the attach token"));
        });
        CHECK(!registered);
        CHECK(!router.has_method(method));
    }
    CHECK(router.method_count() == 0);

    // ...and asking for one over the wire is reported as PERMANENTLY DENIED, distinct from merely
    // absent, because only the first is worth alerting on.
    for (const std::string& method : shell::forbidden_bridge_methods())
    {
        const shell::BridgeDispatch out = router.dispatch(request(method.c_str()));
        CHECK(out.refused());
        CHECK(out.reject == shell::BridgeReject::forbidden_method);
    }

    // The registration guard rejects the other degenerate bindings too.
    CHECK(!router.register_method("", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::ok(contract::Json());
    }));
    CHECK(!router.register_method("shell.x", nullptr));
    CHECK(router.register_method("shell.x", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::ok(contract::Json());
    }));
    // No silent re-binding: a second registration under one name would make which handler runs a
    // function of registration order.
    CHECK(!router.register_method("shell.x", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::ok(contract::Json());
    }));
}

void test_control_3_secrets_never_leave()
{
    shell::BridgeRouter router;
    router.protect_secret(kFakeToken);
    router.protect_secret(kFakeEndpoint);
    CHECK(router.secret_count() == 2);

    // Four handlers that each try to leak the credential a DIFFERENT way. The scan is over the
    // SERIALIZED response precisely so none of these shapes needs its own special case.
    CHECK(router.register_method("leak.direct", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::ok(contract::Json(kFakeToken));
    }));
    CHECK(router.register_method("leak.nested", [](const shell::BridgeRequest&) {
        contract::Json inner = contract::Json::object();
        inner.set("token", contract::Json(kFakeToken));
        contract::Json outer = contract::Json::object();
        outer.set("deep", inner);
        return shell::BridgeResult::ok(outer);
    }));
    CHECK(router.register_method("leak.in_key", [](const shell::BridgeRequest&) {
        contract::Json value = contract::Json::object();
        // The secret as an object KEY, not a value — a recursive walk over values alone misses it.
        value.set(kFakeToken, contract::Json(1));
        return shell::BridgeResult::ok(value);
    }));
    CHECK(router.register_method("leak.in_error", [](const shell::BridgeRequest&) {
        // Smuggled through a diagnostic string, the way a real leak usually happens.
        return shell::BridgeResult::error("shell.oops",
                                          std::string("could not attach with ") + kFakeEndpoint);
    }));
    // REGRESSION GUARD for a hole this suite actually found: the Windows daemon endpoint is a
    // named pipe (`\\.\pipe\...`), so the SERIALIZER ESCAPES ITS BACKSLASHES and the response text
    // does not contain the raw bytes at all. A scan for the raw form alone let this straight
    // through — on exactly the platform whose credential shape needs escaping. The guard now
    // derives the on-the-wire form with the same serializer and scans for both.
    CHECK(router.register_method("leak.escaped_value", [](const shell::BridgeRequest&) {
        contract::Json value = contract::Json::object();
        value.set("endpoint", contract::Json(kFakeEndpoint));
        return shell::BridgeResult::ok(value);
    }));

    for (const char* method :
         {"leak.direct", "leak.nested", "leak.in_key", "leak.in_error", "leak.escaped_value"})
    {
        const shell::BridgeDispatch out = router.dispatch(request(method));
        CHECK(out.refused());
        CHECK(out.reject == shell::BridgeReject::secret_blocked);
        // THE assertion: the credential is not in what the renderer receives — in EITHER form.
        CHECK(!shelltest::mentions(out.response, kFakeToken));
        CHECK(!shelltest::mentions(out.response, kFakeEndpoint));
        // ...and not as the JSON-escaped text the serializer would have written.
        const std::string escaped = contract::Json(kFakeEndpoint).dump();
        CHECK(!shelltest::mentions(out.response, escaped.substr(1, escaped.size() - 2).c_str()));
    }
    // Every save is counted, so an attempted leak is observable rather than silent.
    CHECK(router.secrets_blocked() == 5);

    // A response that does NOT carry a secret is unaffected — the guard is a filter, not a blanket.
    CHECK(router.register_method("shell.safe", [](const shell::BridgeRequest&) {
        return shell::BridgeResult::ok(contract::Json("nothing sensitive here"));
    }));
    const shell::BridgeDispatch safe = router.dispatch(request("shell.safe"));
    CHECK(!safe.refused());
    CHECK(router.secrets_blocked() == 5);

    // Degenerate "secrets" are ignored: scanning for a 1-2 character string would block every
    // response and make the control useless in practice.
    shell::BridgeRouter tiny;
    tiny.protect_secret("");
    tiny.protect_secret("ab");
    CHECK(tiny.secret_count() == 0);
    // ...and the same secret twice is stored once.
    shell::BridgeRouter deduped;
    deduped.protect_secret(kFakeToken);
    deduped.protect_secret(kFakeToken);
    CHECK(deduped.secret_count() == 1);
}

void test_control_1_router_carries_no_credential()
{
    // CONTROL 1 is a TYPE property, so it is asserted structurally: a default-constructed router is
    // fully usable, which is only possible because it needs no client, socket, token or project
    // root to exist. Nothing here can reach a credential because there is nothing to reach.
    shell::BridgeRouter router;
    CHECK(router.method_count() == 0);
    CHECK(router.secret_count() == 0);
    CHECK(router.served() == 0);

    // A handler sees ONLY the request — whatever else it can reach, the Shell chose to give it at
    // registration, which is where that decision is reviewable.
    CHECK(router.register_method("shell.what_can_i_see", [](const shell::BridgeRequest& request) {
        contract::Json value = contract::Json::object();
        value.set("method", contract::Json(request.method));
        value.set("param_count", contract::Json(static_cast<std::int64_t>(request.params.size())));
        return shell::BridgeResult::ok(value);
    }));
    const shell::BridgeDispatch out =
        router.dispatch(request("shell.what_can_i_see", "{\"a\":1,\"b\":2}"));
    CHECK(!out.refused());
    const contract::Json response = contract::Json::parse(out.response);
    CHECK(response.at("result").at("param_count").as_int() == 2);
}

// ------------------------------------------------------------------------------ origin trust

void test_only_editor_core_origin_is_trusted()
{
    // The privileged bridge accepts queries from editor-core's origin and NOWHERE else. A
    // sandboxed third-party panel (04 §5) lives on its own `context-ext://` origin and reaches the
    // daemon through the SCOPED panel bridge instead.
    CHECK(shell::is_trusted_bridge_origin("context-editor://app"));
    CHECK(shell::is_trusted_bridge_origin("context-editor://app/"));
    CHECK(shell::is_trusted_bridge_origin("context-editor://app/index.html"));
    CHECK(shell::is_trusted_bridge_origin("context-editor://app/sub/panel.html?x=1"));
    CHECK(shell::is_trusted_bridge_origin("context-editor://app#frag"));

    CHECK(!shell::is_trusted_bridge_origin(""));
    CHECK(!shell::is_trusted_bridge_origin("about:blank"));
    CHECK(!shell::is_trusted_bridge_origin("file:///c:/app/index.html"));
    CHECK(!shell::is_trusted_bridge_origin("https://example.com/"));
    CHECK(!shell::is_trusted_bridge_origin("context-ext://some-package/panel.html"));
    CHECK(!shell::is_trusted_bridge_origin("context-editor://ipc"));
    // A bare prefix test would accept this — the character after the origin must be a separator.
    CHECK(!shell::is_trusted_bridge_origin("context-editor://appliance/evil.html"));
    CHECK(!shell::is_trusted_bridge_origin("context-editor://app.evil.com/x"));
    CHECK(!shell::is_trusted_bridge_origin("https://context-editor://app/"));
}

void test_envelope_builders()
{
    const contract::Json value = contract::Json("ok");
    const contract::Json result = contract::Json::parse(shell::build_bridge_result(7, value));
    CHECK(result.at("jsonrpc").as_string() == "2.0");
    CHECK(result.at("id").as_int() == 7);
    CHECK(result.at("result").as_string() == "ok");

    const contract::Json error =
        contract::Json::parse(shell::build_bridge_error(9, -32600, "bridge.bad_jsonrpc", "nope"));
    CHECK(error.at("id").as_int() == 9);
    CHECK(error.at("error").at("code").as_int() == -32600);
    CHECK(error.at("error").at("message").as_string() == "nope");
    CHECK(error.at("error").at("data").at("reason").as_string() == "bridge.bad_jsonrpc");

    // Every reject classification has a distinct, non-empty string — they are the machine-readable
    // half of the refusal and a duplicate would make two causes indistinguishable.
    const shell::BridgeReject all[] = {
        shell::BridgeReject::none,           shell::BridgeReject::too_large,
        shell::BridgeReject::not_json,       shell::BridgeReject::not_object,
        shell::BridgeReject::bad_jsonrpc,    shell::BridgeReject::bad_id,
        shell::BridgeReject::bad_method,     shell::BridgeReject::bad_params,
        shell::BridgeReject::unknown_method, shell::BridgeReject::forbidden_method,
        shell::BridgeReject::handler_threw,  shell::BridgeReject::secret_blocked,
    };
    for (std::size_t i = 0; i < std::size(all); ++i)
    {
        const std::string a = shell::to_string(all[i]);
        CHECK(!a.empty());
        for (std::size_t j = i + 1; j < std::size(all); ++j)
        {
            CHECK(a != std::string(shell::to_string(all[j])));
        }
    }
}

// ------------------------------------------------------------------------------ boot handshake

void test_boot_handshake()
{
    shell::BridgeRouter router;
    shell::ShellHandshake handshake(shell::make_handshake_nonce());
    CHECK(handshake.install(router));
    CHECK(!handshake.hello_received());
    CHECK(!handshake.complete());

    // Step 1: JS -> native.
    const shell::BridgeDispatch hello = router.dispatch(
        request("shell.hello", "{\"protocolMajor\":1,\"rpcMethodCount\":7}"));
    CHECK(!hello.refused());
    CHECK(handshake.hello_received());
    // ...still not complete: the request path alone proves only half the channel.
    CHECK(!handshake.complete());
    CHECK(shelltest::mentions(handshake.client_summary(), "protocolMajor"));

    // Step 2: the reply carries the nonce.
    const contract::Json reply = contract::Json::parse(hello.response);
    const std::string nonce = reply.at("result").at("nonce").as_string();
    CHECK(nonce == handshake.nonce());
    CHECK(!nonce.empty());
    CHECK(reply.at("result").at("endpoint").as_string() == std::string(shell::kIpcEndpoint));

    // Step 3: JS echoes it back — only NOW is the round trip proven.
    const std::string ready =
        std::string("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shell.ready\",\"params\":{\"nonce\":\"") +
        nonce + "\"}}";
    CHECK(!router.dispatch(ready).refused());
    CHECK(handshake.complete());
    CHECK(handshake.nonce_mismatches() == 0);

    // A wrong / missing / replayed nonce does NOT complete a handshake.
    shell::BridgeRouter fresh;
    shell::ShellHandshake strict(shell::make_handshake_nonce());
    CHECK(strict.install(fresh));
    for (const char* params : {"{\"nonce\":\"wrong\"}", "{}", "{\"nonce\":42}",
                               "{\"nonce\":null}"})
    {
        const shell::BridgeDispatch out = fresh.dispatch(request("shell.ready", params));
        // A handler-level refusal, not an envelope refusal — the message was well-formed.
        CHECK(!out.refused());
        const contract::Json response = contract::Json::parse(out.response);
        CHECK(response.contains("error"));
        CHECK(response.at("error").at("data").at("reason").as_string() == "shell.nonce_mismatch");
    }
    CHECK(!strict.complete());
    CHECK(strict.nonce_mismatches() == 4);
    // The stale nonce from the OTHER handshake must not work either.
    CHECK(!fresh.dispatch(request("shell.ready",
                                  (std::string("{\"nonce\":\"") + nonce + "\"}").c_str()))
               .refused());
    CHECK(!strict.complete());

    // Nonces are distinct per boot, so a stale renderer cannot complete a fresh Shell's handshake.
    CHECK(shell::make_handshake_nonce() != shell::make_handshake_nonce());

    // Installing twice onto one router is refused (the second bind collides) rather than silently
    // shadowing the first.
    shell::ShellHandshake duplicate(shell::make_handshake_nonce());
    CHECK(!duplicate.install(router));
}

} // namespace

int main()
{
    test_round_trip();
    test_handler_error_is_not_a_refusal();
    test_malformed_input_is_refused_not_fatal();
    test_size_cap();
    test_deep_nesting_is_contained();
    test_control_1_router_carries_no_credential();
    test_control_2_forbidden_methods_cannot_be_registered();
    test_control_3_secrets_never_leave();
    test_only_editor_core_origin_is_trusted();
    test_boot_handshake();
    test_envelope_builders();
    SHELL_TEST_MAIN_END();
}
