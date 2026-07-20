// The privileged native<->JS IPC bridge — see ipc_bridge.h for the model and the three
// token-isolation controls this file implements.

#include "context/editor/shell/ipc_bridge.h"

#include "context/editor/shell/app_scheme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

namespace context::editor::shell
{
namespace
{

// JSON-RPC 2.0's own reserved codes. Deliberately NOT the R-CLI-008 error catalog: that catalog is
// the DAEMON's frozen, additive-only contract surface (protocolMajor 1), and the bridge's refusals
// are Shell-local transport facts about a message that never reached the daemon at all. Minting
// catalog codes for them would put renderer-input validation into the engine's public diagnostic
// vocabulary, where it would then be frozen forever. The machine-readable classification the caller
// actually needs travels in `error.data.reason`.
constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;

// JavaScript numbers are IEEE-754 doubles, so an id beyond 2^53 cannot round-trip through the
// renderer. Rejecting it here is more honest than silently answering a different id than the one
// the caller believes it sent.
constexpr double kMaxSafeInteger = 9007199254740991.0;

// Below this length a "secret" is not distinctive enough to scan for — every response would match.
// Real credentials (the D20 attach token) are far longer.
constexpr std::size_t kMinProtectedSecretLength = 8;

} // namespace

// ASCII-only lowercase + outer-whitespace trim, so control 2's forbidden-name comparison is not
// defeated by a spelling variant ("Attach", "attach "). Deliberately ASCII-only: these names are
// ASCII by construction, and a locale-aware fold would be a different (and less predictable) rule.
std::string normalize_method_name(std::string_view method)
{
    std::size_t begin = 0;
    std::size_t end = method.size();
    const auto is_space = [](unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d); };
    while (begin < end && is_space(static_cast<unsigned char>(method[begin])))
    {
        ++begin;
    }
    while (end > begin && is_space(static_cast<unsigned char>(method[end - 1])))
    {
        --end;
    }
    std::string out(method.substr(begin, end - begin));
    for (char& c : out)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z')
        {
            c = static_cast<char>(uc - 'A' + 'a');
        }
    }
    return out;
}

// Structural nesting depth of a JSON text, measured LEXICALLY — brackets inside string literals do
// not count, and an escaped quote does not end a string. Used to refuse a message BEFORE handing it
// to the recursive-descent parser (see kMaxBridgeMessageDepth); a post-parse walk would be too late,
// because the overflow happens DURING the parse.
std::size_t max_structural_depth(std::string_view text)
{
    std::size_t depth = 0;
    std::size_t deepest = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text)
    {
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '{' || c == '[')
        {
            ++depth;
            deepest = std::max(deepest, depth);
        }
        else if ((c == '}' || c == ']') && depth > 0)
        {
            --depth;
        }
    }
    return deepest;
}

// Control 2's predicate, shared by `register_method` and `dispatch` so the two can never disagree
// about what "forbidden" means. Matches the normalized name exactly OR as a DOTTED PREFIX, so
// `instance` also denies `instance.read`, `instance.token`, … — a credential verb is not made
// routable by appending a segment.
bool is_forbidden_bridge_method(std::string_view method);

const std::vector<std::string>& forbidden_bridge_methods()
{
    static const std::vector<std::string> kForbidden = {
        // Attaching is the SHELL's own authenticated act (D20). Routing it from editor-core would
        // mean the renderer choosing the token — or being handed one back.
        "attach",
        "attach.token",
        // `.editor/instance.json` is literally where the attach token lives (08 §1). Any verb that
        // reads or reports it is a credential read by another name.
        "instance",
        "instance.read",
        "instance.info",
        // The daemon endpoint (pipe/socket path) is the other half of the credential pair: with it
        // a rogue local process has somewhere to present a stolen token.
        "daemon.endpoint",
        "daemon.socket",
        // The Shell's own attach options carry the scope grant AND the token.
        "shell.attach_options",
    };
    return kForbidden;
}

bool is_forbidden_bridge_method(std::string_view method)
{
    const std::string key = normalize_method_name(method);
    return std::any_of(forbidden_bridge_methods().begin(), forbidden_bridge_methods().end(),
                       [&](const std::string& forbidden) {
                           if (key == forbidden)
                           {
                               return true;
                           }
                           // Dotted-prefix: `instance` denies `instance.anything`, but NOT a
                           // different verb that merely starts with the same letters
                           // (`instanced.list` stays routable).
                           return key.size() > forbidden.size() &&
                                  key.compare(0, forbidden.size(), forbidden) == 0 &&
                                  key[forbidden.size()] == '.';
                       });
}

BridgeResult BridgeResult::ok(contract::Json value)
{
    BridgeResult out;
    out.value = std::move(value);
    return out;
}

BridgeResult BridgeResult::error(std::string code, std::string message)
{
    BridgeResult out;
    // A caller that passes an empty code would otherwise produce a SUCCESS envelope carrying an
    // error message — fail loudly into a generic code instead.
    out.error_code = code.empty() ? std::string("bridge.handler_error") : std::move(code);
    out.error_message = std::move(message);
    return out;
}

const char* to_string(BridgeReject reject)
{
    switch (reject)
    {
    case BridgeReject::none:
        return "none";
    case BridgeReject::too_large:
        return "bridge.too_large";
    case BridgeReject::too_deep:
        return "bridge.too_deep";
    case BridgeReject::not_json:
        return "bridge.not_json";
    case BridgeReject::not_object:
        return "bridge.not_object";
    case BridgeReject::bad_jsonrpc:
        return "bridge.bad_jsonrpc";
    case BridgeReject::bad_id:
        return "bridge.bad_id";
    case BridgeReject::bad_method:
        return "bridge.bad_method";
    case BridgeReject::bad_params:
        return "bridge.bad_params";
    case BridgeReject::unknown_method:
        return "bridge.unknown_method";
    case BridgeReject::forbidden_method:
        return "bridge.forbidden_method";
    case BridgeReject::handler_threw:
        return "bridge.handler_threw";
    case BridgeReject::secret_blocked:
        return "bridge.secret_blocked";
    }
    return "bridge.unknown";
}

std::string build_bridge_result(std::int64_t id, const contract::Json& value)
{
    contract::Json envelope = contract::Json::object();
    envelope.set("jsonrpc", contract::Json("2.0"));
    envelope.set("id", contract::Json(id));
    envelope.set("result", value);
    return envelope.dump();
}

std::string build_bridge_error(std::int64_t id, int code, const std::string& reason,
                               const std::string& message)
{
    contract::Json data = contract::Json::object();
    data.set("reason", contract::Json(reason));

    contract::Json error = contract::Json::object();
    error.set("code", contract::Json(static_cast<std::int64_t>(code)));
    error.set("message", contract::Json(message));
    error.set("data", data);

    contract::Json envelope = contract::Json::object();
    envelope.set("jsonrpc", contract::Json("2.0"));
    envelope.set("id", contract::Json(id));
    envelope.set("error", error);
    return envelope.dump();
}

bool is_trusted_bridge_origin(std::string_view frame_url)
{
    const std::string_view origin{kAppOrigin};
    if (frame_url == origin)
    {
        return true;
    }
    // `context-editor://app/...` — the ONLY accepted shape. A prefix test alone would also accept
    // `context-editor://appliance/...`, so the character after the origin must be a separator.
    if (frame_url.size() > origin.size() && frame_url.substr(0, origin.size()) == origin)
    {
        const char next = frame_url[origin.size()];
        return next == '/' || next == '?' || next == '#';
    }
    return false;
}

bool BridgeRouter::register_method(std::string method, BridgeHandler handler)
{
    if (method.empty() || handler == nullptr)
    {
        return false;
    }
    // CONTROL 2 (see the header): the credential-bearing methods are refused outright, so wiring
    // one through is a build-visible `false` rather than a working leak.
    if (is_forbidden_bridge_method(method))
    {
        return false;
    }
    if (has_method(method))
    {
        return false;
    }
    handlers_.emplace_back(std::move(method), std::move(handler));
    return true;
}

bool BridgeRouter::protect_secret(std::string secret)
{
    // REPORTED, not silent. Control 3 is the backstop that does not depend on anyone reasoning
    // correctly about controls 1 and 2, so a credential that silently failed to register would
    // disarm it with no counter and no log to notice by. Nothing upstream enforces a minimum token
    // length (`guard_shell_attach` rejects only an EMPTY token, and `parse_instance_document`
    // accepts any non-empty string), so a daemon publishing a short token is reachable, not
    // hypothetical — the caller is told and decides.
    if (secret.size() < kMinProtectedSecretLength)
    {
        return false;
    }
    const bool already = std::any_of(secrets_.begin(), secrets_.end(),
                                     [&](const ProtectedSecret& s) { return s.raw == secret; });
    if (already)
    {
        // Already protected: the postcondition the caller cares about (this value cannot leave
        // through the bridge) holds, so this is success, not a refusal.
        return true;
    }

    ProtectedSecret entry;
    entry.raw = std::move(secret);
    // Derive the on-the-wire form with the SAME serializer that writes responses (see the header's
    // ProtectedSecret note on why the raw bytes alone are not enough). Dumping a JSON string yields
    // the escaped text WRAPPED IN QUOTES; the quotes are stripped so the needle is the escaped
    // CONTENT and therefore still matches when the secret is embedded in a longer string.
    std::string dumped = contract::Json(entry.raw).dump();
    if (dumped.size() >= 2)
    {
        entry.encoded = dumped.substr(1, dumped.size() - 2);
    }
    secrets_.push_back(std::move(entry));
    return true;
}

bool BridgeRouter::has_method(const std::string& method) const
{
    return std::any_of(handlers_.begin(), handlers_.end(),
                       [&](const auto& entry) { return entry.first == method; });
}

bool BridgeRouter::carries_secret(const std::string& response) const
{
    return std::any_of(secrets_.begin(), secrets_.end(), [&](const ProtectedSecret& secret) {
        if (response.find(secret.raw) != std::string::npos)
        {
            return true;
        }
        return !secret.encoded.empty() && response.find(secret.encoded) != std::string::npos;
    });
}

BridgeDispatch BridgeRouter::refuse(std::int64_t id, BridgeReject reject, int json_rpc_code,
                                    const std::string& message)
{
    ++refused_;
    last_reject_ = reject;
    BridgeDispatch out;
    out.reject = reject;
    out.response = build_bridge_error(id, json_rpc_code, to_string(reject), message);
    return out;
}

BridgeDispatch BridgeRouter::dispatch(std::string_view message)
{
    // The cap is checked FIRST, on the raw view, before a single byte is copied or parsed — the
    // whole point is not to allocate proportionally to untrusted input.
    if (message.size() > kMaxBridgeMessageBytes)
    {
        return refuse(0, BridgeReject::too_large, kJsonRpcInvalidRequest,
                      "message exceeds the bridge size limit");
    }

    // Depth SECOND, and still before the parse. The size cap does not bound the parse: the contract
    // parser is recursive descent with no depth limit, so ~1e6 `[` inside a 1 MiB message overflows
    // the stack DURING descent — a SIGSEGV, which the try/catch below cannot contain. Measuring the
    // nesting lexically first is what makes "rejected without crashing the Shell" true.
    if (max_structural_depth(message) > kMaxBridgeMessageDepth)
    {
        return refuse(0, BridgeReject::too_deep, kJsonRpcInvalidRequest,
                      "message nesting exceeds the bridge depth limit");
    }

    contract::Json request;
    try
    {
        request = contract::Json::parse(std::string(message));
    }
    catch (const std::exception&)
    {
        // The parser's message carries a byte offset into the ATTACKER'S input; echoing it back
        // would be a parser-probing oracle. The classification is enough.
        return refuse(0, BridgeReject::not_json, kJsonRpcParseError, "message is not valid JSON");
    }
    catch (...)
    {
        return refuse(0, BridgeReject::not_json, kJsonRpcParseError, "message is not valid JSON");
    }

    if (!request.is_object())
    {
        return refuse(0, BridgeReject::not_object, kJsonRpcInvalidRequest,
                      "message is not a JSON-RPC object");
    }

    // The id is read BEFORE the rest is validated, so a refusal can be correlated with the request
    // that caused it — a caller whose refusals all came back as id 0 could not match them up.
    const contract::Json& id_value = request.at("id");
    if (!id_value.is_number())
    {
        return refuse(0, BridgeReject::bad_id, kJsonRpcInvalidRequest,
                      "request id must be a number");
    }
    const double id_number = id_value.as_number();
    // NaN/inf cannot come from the parser today, but a static_cast of one to int64 is UB, so the
    // guard is unconditional rather than reasoned away.
    if (!std::isfinite(id_number) || std::fabs(id_number) > kMaxSafeInteger)
    {
        return refuse(0, BridgeReject::bad_id, kJsonRpcInvalidRequest,
                      "request id is outside the safe integer range");
    }
    const std::int64_t id = id_value.as_int();

    const contract::Json& jsonrpc = request.at("jsonrpc");
    if (!jsonrpc.is_string() || jsonrpc.as_string() != "2.0")
    {
        return refuse(id, BridgeReject::bad_jsonrpc, kJsonRpcInvalidRequest,
                      "jsonrpc must be the string \"2.0\"");
    }

    const contract::Json& method_value = request.at("method");
    if (!method_value.is_string() || method_value.as_string().empty())
    {
        return refuse(id, BridgeReject::bad_method, kJsonRpcInvalidRequest,
                      "method must be a non-empty string");
    }
    const std::string& method = method_value.as_string();

    // CONTROL 2's runtime half. `register_method` already refuses these, so reaching a forbidden
    // name means nothing is bound to it and `unknown_method` would be the honest answer — but the
    // two are reported DIFFERENTLY on purpose: "you asked for something that is permanently denied"
    // is a distinct fact from "you asked for something that does not exist", and only the first is
    // worth alerting on.
    if (is_forbidden_bridge_method(method))
    {
        return refuse(id, BridgeReject::forbidden_method, kJsonRpcMethodNotFound,
                      "method is not reachable from editor-core");
    }

    BridgeRequest call;
    call.id = id;
    call.method = method;
    if (request.contains("params"))
    {
        const contract::Json& params = request.at("params");
        // Absent is fine; present-but-not-an-object is not. Accepting an array or a scalar here
        // would push the shape question into every handler, which is where it gets forgotten.
        if (!params.is_object())
        {
            return refuse(id, BridgeReject::bad_params, kJsonRpcInvalidParams,
                          "params must be an object when present");
        }
        call.params = params;
    }
    else
    {
        call.params = contract::Json::object();
    }

    const auto entry = std::find_if(handlers_.begin(), handlers_.end(),
                                    [&](const auto& e) { return e.first == method; });
    if (entry == handlers_.end())
    {
        // DENY-BY-DEFAULT: only what the Shell explicitly bound is reachable.
        return refuse(id, BridgeReject::unknown_method, kJsonRpcMethodNotFound,
                      "no such bridge method");
    }

    // COPIED, not called through the iterator: a handler is free to call `register_method`, whose
    // `handlers_.emplace_back` can reallocate the vector and destroy the very callable that is
    // executing. Only Shell-authored handlers can register, so this is a footgun rather than a
    // renderer-reachable hole — but a std::function copy is nothing against a dispatch, and it
    // makes the call reentrancy-safe by construction rather than by convention.
    const BridgeHandler handler = entry->second;

    BridgeResult result;
    try
    {
        result = handler(call);
    }
    catch (const std::exception&)
    {
        // A handler that throws must not unwind into CEF's message-router callback — an exception
        // crossing that boundary takes the Shell (and the whole editor) down. Contained here.
        return refuse(id, BridgeReject::handler_threw, kJsonRpcInternalError,
                      "the bridge handler failed");
    }
    catch (...)
    {
        return refuse(id, BridgeReject::handler_threw, kJsonRpcInternalError,
                      "the bridge handler failed");
    }

    const std::string response = result.error_code.empty()
                                     ? build_bridge_result(id, result.value)
                                     : build_bridge_error(id, kJsonRpcInternalError,
                                                          result.error_code, result.error_message);

    // CONTROL 3 (see the header): the last gate before anything reaches the renderer. Scanning the
    // SERIALIZED response rather than the Json tree is deliberate — it catches a secret wherever it
    // ended up (a nested value, a key, an error message, a concatenated diagnostic string), with no
    // recursive walk to get wrong.
    if (carries_secret(response))
    {
        // The CLASSIFICATION stays internal. Telling the renderer "that call produced a protected
        // credential" is a map of which methods to attack next — the same probe-oracle reasoning the
        // sibling asset resolver states explicitly ("the REASON is logged, never sent"). Outwardly
        // this is indistinguishable from any other handler failure; `secrets_blocked()` remains the
        // operator's channel, and `last_reject_` the test's.
        ++secrets_blocked_;
        ++refused_;
        last_reject_ = BridgeReject::secret_blocked;
        BridgeDispatch out;
        out.reject = BridgeReject::secret_blocked;
        out.response = build_bridge_error(id, kJsonRpcInternalError,
                                          to_string(BridgeReject::handler_threw),
                                          "the bridge handler failed");
        return out;
    }

    ++served_;
    last_reject_ = BridgeReject::none;
    BridgeDispatch out;
    out.response = response;
    return out;
}

// ---------------------------------------------------------------------------- the boot handshake

std::string make_handshake_nonce()
{
    // steady_clock ticks + a process-local counter. Distinct per boot, which is the whole
    // requirement (see the header: this is a round-trip marker, not a credential).
    static std::uint64_t counter = 0;
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return "hs-" + std::to_string(ticks) + "-" + std::to_string(++counter);
}

ShellHandshake::ShellHandshake(std::string nonce) : nonce_(std::move(nonce)) {}

bool ShellHandshake::install(BridgeRouter& router)
{
    // `this` is captured, so the handshake must outlive the router's handlers. Both are owned by
    // the same boot scope in every caller (the app and the smoke), which is the shape that keeps
    // this safe; a handshake with a shorter life than its router would be a use-after-free.
    const bool hello = router.register_method("shell.hello", [this](const BridgeRequest& request) {
        hello_received_ = true;
        // Recorded verbatim rather than parsed into fields: this is a diagnostic breadcrumb about
        // what the renderer thinks it is, and giving it a schema would invite treating renderer
        // claims as facts.
        client_summary_ = request.params.dump();

        contract::Json value = contract::Json::object();
        value.set("nonce", contract::Json(nonce_));
        // The channel's own identity, echoed so the client can assert it is talking to the Shell it
        // expects. NOT the daemon endpoint — that one is a credential and is on the forbidden list.
        value.set("endpoint", contract::Json(kIpcEndpoint));
        return BridgeResult::ok(value);
    });

    const bool ready = router.register_method("shell.ready", [this](const BridgeRequest& request) {
        const contract::Json& echoed = request.params.at("nonce");
        if (!echoed.is_string() || echoed.as_string() != nonce_)
        {
            ++nonce_mismatches_;
            return BridgeResult::error("shell.nonce_mismatch",
                                       "shell.ready did not echo the handshake nonce");
        }
        complete_ = true;
        contract::Json value = contract::Json::object();
        value.set("acknowledged", contract::Json(true));
        return BridgeResult::ok(value);
    });

    return hello && ready;
}

} // namespace context::editor::shell
