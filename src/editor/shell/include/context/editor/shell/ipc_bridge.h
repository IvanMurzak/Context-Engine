// The privileged native<->JS IPC bridge (M9 e05c, design 04 §1 / 08 §1-§2) — envelope parsing,
// deny-by-default routing, and the token-isolation controls, with NO CEF in any of it.
//
// WHAT THIS IS. editor-core runs in a renderer process and has NO daemon socket and NO attach
// token; the Shell holds both. This bridge is the ONLY path from editor-core to either the daemon
// or the Shell's own state (window registry, drag sessions, region maps). It is therefore
// PRIVILEGED, and every inbound message is UNTRUSTED INPUT FROM RENDERER-PROCESS CONTENT — a
// renderer that a hostile page or a Chromium bug has taken over sends bytes down exactly this
// channel. It is written to be un-crashable and un-leaky against that input, not merely correct
// against a well-behaved caller.
//
// CEF-FREE, for the reason browser.h and app_scheme.h give: CEF is a CI-only dependency path, so
// logic living inside the message-router handler would be exercised by nothing that runs locally.
// The adversarial-input suite (tests/test_ipc_bridge.cpp) runs on all three default `build` legs.
//
// THE ENVELOPE MIRRORS THE DAEMON'S (JSON-RPC 2.0), deliberately (design: "message envelopes carry
// the same shape as the daemon's so e05d's client can layer on cleanly"). One request shape, one
// response shape, one error shape, whether the call is served by the Shell or forwarded onward.
//
// ======================= THE THREE TOKEN-ISOLATION CONTROLS (08 §1) =======================
//
// "The Shell holds the daemon socket and the attach token; editor-core MUST NEVER see either."
// That is enforced by three MECHANISMS, not by a comment:
//
//   1. NO CREDENTIAL IS REACHABLE FROM HERE BY CONSTRUCTION. A `BridgeRouter` holds handlers and
//      nothing else — no Client, no socket, no token, no project root. A handler receives ONLY the
//      request. Whatever a handler can see, the Shell chose to hand it at registration.
//
//   2. `register_method` REFUSES the credential-bearing methods outright (kForbiddenMethods). This
//      is the structural half: a future maintainer who wires `attach` through the bridge does not
//      get a working leak and a review comment — they get `false` back and an unroutable method.
//      Routing is allowlist-only regardless (an unregistered method is `unknown_method`), so this
//      is a second lock on the one door most likely to be opened by accident.
//
//   3. EVERY OUTBOUND RESPONSE IS SCANNED FOR THE REGISTERED SECRETS, and a hit is replaced with an
//      internal error rather than sent. This is the backstop that does not depend on anyone
//      reasoning correctly about (1) and (2): the Shell calls `protect_secret(token)` at boot, and
//      from then on the token cannot leave through this channel VERBATIM — in either its raw or its
//      JSON-serialized form — no matter which handler produced it or how deeply nested it is in the
//      payload. Those two forms are exhaustive for a CONTIGUOUS appearance, because `encoded` is
//      derived with the SAME serializer that writes the response. What this control does NOT reach:
//      a secret split across two fields, partially echoed, or re-encoded by a handler (base64,
//      case-folded, separator-flipped) — a substring scan cannot see those, and that is precisely
//      what controls (1) and (2) are for. `secrets_blocked()` counts the saves, so a leak that IS
//      attempted is observable rather than silent.
//
// =========================================================================================

#pragma once

#include "context/editor/contract/json.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell
{

// The largest inbound message the bridge will even parse. A renderer can send an arbitrarily large
// string; without a cap, parsing one is an unbounded allocation driven by untrusted input.
inline constexpr std::size_t kMaxBridgeMessageBytes = 1u << 20; // 1 MiB

// Maximum structural nesting the bridge will hand to the parser.
//
// THE SIZE CAP ALONE DOES NOT BOUND THE PARSE. `contract::Json::parse` is recursive descent with no
// depth limit of its own (parse_value -> parse_array/parse_object -> parse_value), so a message that
// is comfortably UNDER kMaxBridgeMessageBytes — a 60-byte envelope prefix followed by ~1e6 `[` — is
// ~1e6 recursion levels deep and overflows the stack during descent. A stack overflow is a SIGSEGV /
// EXCEPTION_STACK_OVERFLOW, NOT a C++ exception, so neither `catch (const std::exception&)` nor
// `catch (...)` around the parse can contain it: the renderer would kill the Shell, defeating the
// e05c DoD line "malformed/hostile inbound bridge messages rejected without crashing the Shell".
// The depth is therefore measured LEXICALLY, before the parser is ever called.
//
// 64 is far above anything the daemon's envelopes reach (JSON-RPC params nest a handful deep) and far
// below any platform's stack limit.
inline constexpr std::size_t kMaxBridgeMessageDepth = 64;

// Methods that may NEVER be routed from editor-core, whatever anyone later registers (control 2
// above). These are the credential-bearing / credential-adjacent surfaces: attaching is the Shell's
// own authenticated act (D20), and the instance document is where the attach token LIVES
// (`.editor/instance.json`, 08 §1).
[[nodiscard]] const std::vector<std::string>& forbidden_bridge_methods();

// ------------------------------------------------------------------------------- the envelope

struct BridgeRequest
{
    std::int64_t id = 0;
    std::string method;
    contract::Json params; // always an object (a non-object `params` is rejected before dispatch)
};

// What a handler returns. A non-empty `error_code` makes it an error response; `value` is then
// ignored.
struct BridgeResult
{
    contract::Json value;
    std::string error_code;
    std::string error_message;

    [[nodiscard]] static BridgeResult ok(contract::Json value);
    [[nodiscard]] static BridgeResult error(std::string code, std::string message);
};

using BridgeHandler = std::function<BridgeResult(const BridgeRequest&)>;

// Why a message was refused. Every value is a REFUSAL — `none` means the message was dispatched to
// a handler (whose own error is not a refusal: the envelope was valid).
enum class BridgeReject
{
    none,
    too_large,        // over kMaxBridgeMessageBytes
    too_deep,         // nesting over kMaxBridgeMessageDepth — refused BEFORE the parse, see above
    not_json,         // unparseable
    not_object,       // valid JSON, but not an object (an array, a bare string, null, …)
    bad_jsonrpc,      // missing/wrong "jsonrpc"
    bad_id,           // missing "id", or one that is not a safe integer
    bad_method,       // missing/empty/non-string "method"
    bad_params,       // "params" present but not an object
    unknown_method,   // well-formed, but nothing is registered under it (deny-by-default)
    forbidden_method, // on kForbiddenMethods — never routable
    handler_threw,    // a handler threw; contained rather than propagated
    secret_blocked,   // control 3 fired: the response carried a registered secret
};

[[nodiscard]] const char* to_string(BridgeReject reject);

struct BridgeDispatch
{
    // The response envelope to hand back to the renderer. ALWAYS a well-formed JSON-RPC response,
    // including for every refusal above — a renderer that sent garbage still gets a legible answer
    // rather than a hang.
    std::string response;
    BridgeReject reject = BridgeReject::none;

    [[nodiscard]] bool refused() const { return reject != BridgeReject::none; }
};

// --------------------------------------------------------------------------------- the router

class BridgeRouter
{
public:
    BridgeRouter() = default;

    // Neither copyable NOR movable, and the move deletion is DELIBERATE rather than a side effect
    // of deleting the copy: the CEF message-router handler holds a RAW POINTER to the Shell's
    // router for the life of the browser (it cannot own it — the router outlives every browser and
    // is shared by the windows). A movable router would let `auto r = make_router();` silently
    // relocate one out from under that pointer, which is a use-after-move that only shows up as a
    // crash in the renderer's query path. Construct it in place and hand out references.
    BridgeRouter(const BridgeRouter&) = delete;
    BridgeRouter& operator=(const BridgeRouter&) = delete;
    BridgeRouter(BridgeRouter&&) = delete;
    BridgeRouter& operator=(BridgeRouter&&) = delete;

    // Bind a method. Returns false — and binds NOTHING — when `method` is empty, already bound, or
    // on `forbidden_bridge_methods()` (control 2). The caller is expected to check: a silently
    // dropped registration would present as a mysteriously unknown method at runtime.
    [[nodiscard]] bool register_method(std::string method, BridgeHandler handler);

    // Register a value that must never appear in an outbound response (control 3). Called by the
    // Shell with the attach token and the daemon endpoint. Empty and very short values are REFUSED:
    // scanning for a 1-2 character "secret" would match ordinary payload text and turn every
    // response into a blocked one.
    //
    // Returns false when the value was NOT taken under protection (empty / shorter than
    // kMinProtectedSecretLength), true when it is protected — including when it already was. The
    // caller MUST check: control 3 is the backstop for the other two, so a credential that silently
    // failed to register disarms it with nothing to notice by. Nothing upstream guarantees a minimum
    // token length, so this is reachable rather than theoretical.
    [[nodiscard]] bool protect_secret(std::string secret);

    // Parse, validate, route, and serialize one message. NEVER throws: every failure path — invalid
    // JSON, a hostile shape, a handler that throws — produces a refusal envelope.
    [[nodiscard]] BridgeDispatch dispatch(std::string_view message);

    [[nodiscard]] bool has_method(const std::string& method) const;
    [[nodiscard]] std::size_t method_count() const { return handlers_.size(); }
    [[nodiscard]] std::size_t secret_count() const { return secrets_.size(); }

    // --- what it saw ------------------------------------------------------------------------------
    [[nodiscard]] std::size_t served() const { return served_; }
    [[nodiscard]] std::size_t refused() const { return refused_; }
    // How many responses control 3 had to blank. NON-ZERO IS A DEFECT, not a success metric: it
    // means a handler tried to return a credential and the backstop caught it.
    [[nodiscard]] std::size_t secrets_blocked() const { return secrets_blocked_; }
    // The last refusal's classification — for the diagnostic channel and the tests.
    [[nodiscard]] BridgeReject last_reject() const { return last_reject_; }

private:
    // One protected value, in BOTH the forms it can appear in on the wire.
    //
    // WHY TWO FORMS — a scan for the raw bytes alone is a HOLE, and a real one. The response is
    // scanned as SERIALIZED JSON, and the serializer escapes backslashes, quotes, control
    // characters and non-ASCII. The Windows daemon endpoint is a named pipe —
    // `\\.\pipe\context-daemon-...` — so its serialized form is `\\\\.\\pipe\\...`, which does NOT
    // contain the raw string as a substring. Scanning only `raw` would therefore have let the
    // endpoint through on exactly the platform whose credential shape needs escaping. `encoded` is
    // produced by the SAME serializer that writes the response, so the two can never disagree
    // about what the escaping is.
    struct ProtectedSecret
    {
        std::string raw;
        std::string encoded;
    };

    [[nodiscard]] BridgeDispatch refuse(std::int64_t id, BridgeReject reject, int json_rpc_code,
                                        const std::string& message);
    // Scan a finished response for every registered secret, in both forms (control 3).
    [[nodiscard]] bool carries_secret(const std::string& response) const;

    std::vector<std::pair<std::string, BridgeHandler>> handlers_;
    std::vector<ProtectedSecret> secrets_;
    std::size_t served_ = 0;
    std::size_t refused_ = 0;
    std::size_t secrets_blocked_ = 0;
    BridgeReject last_reject_ = BridgeReject::none;
};

// ------------------------------------------------------------------------------- envelope builders

// Build the two response shapes. Exposed because the CEF binding needs the error shape for the
// failures it detects BEFORE the router is reached (a query from a frame that is not editor-core's
// origin), and a second hand-rolled copy of the envelope is how the two shapes drift apart.
[[nodiscard]] std::string build_bridge_result(std::int64_t id, const contract::Json& value);
[[nodiscard]] std::string build_bridge_error(std::int64_t id, int code, const std::string& reason,
                                             const std::string& message);

// True when `frame_url` is inside editor-core's own origin (`context-editor://app`). The bridge
// accepts queries from NOWHERE else: a sandboxed third-party panel (04 §5) lives on a different
// `context-ext://` origin and reaches the daemon through the SCOPED panel bridge, never this
// privileged one.
[[nodiscard]] bool is_trusted_bridge_origin(std::string_view frame_url);

// ---------------------------------------------------------------------------- the boot handshake

// The native half of editor-core's boot handshake (e05c), shared by `context_editor` and the live
// CEF smoke so both exercise the SAME code rather than two lookalikes.
//
// WHY A NONCE. A one-way "the bundle called us" ping proves only that the request path works — it
// would pass with a completely broken response path, which is half the channel. So `shell.hello`
// replies with a nonce and `shell.ready` must echo it back: `complete()` is true only when a value
// made the FULL native -> JS -> native round trip.
//
// The nonce is a ROUND-TRIP MARKER, NOT A SECURITY TOKEN. It authenticates nothing (the origin
// check does that, one layer up) and is deliberately not registered as a protected secret — it is
// meant to be sent to the renderer.
class ShellHandshake
{
public:
    // `nonce` must be non-empty; `make_handshake_nonce()` is the usual source.
    explicit ShellHandshake(std::string nonce);

    // Bind `shell.hello` + `shell.ready` on `router`. False when either binding was refused (a name
    // collision), which the caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    [[nodiscard]] bool hello_received() const { return hello_received_; }
    // True once `shell.ready` arrived carrying the MATCHING nonce.
    [[nodiscard]] bool complete() const { return complete_; }
    // How many `shell.ready` calls carried the wrong nonce — non-zero means something replayed or
    // guessed rather than round-tripped.
    [[nodiscard]] std::size_t nonce_mismatches() const { return nonce_mismatches_; }
    // What editor-core reported about itself at `shell.hello` (contract surface it was built
    // against). Empty until then.
    [[nodiscard]] const std::string& client_summary() const { return client_summary_; }
    [[nodiscard]] const std::string& nonce() const { return nonce_; }

private:
    std::string nonce_;
    std::string client_summary_;
    std::size_t nonce_mismatches_ = 0;
    bool hello_received_ = false;
    bool complete_ = false;
};

// A per-boot nonce. Distinctness across runs is what matters (so a stale renderer cannot complete a
// fresh Shell's handshake); it is not required to be unpredictable — see ShellHandshake's note.
[[nodiscard]] std::string make_handshake_nonce();

} // namespace context::editor::shell
