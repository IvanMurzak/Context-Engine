// a17 — TRUST-TIER ADVERSARIAL RED-TEAM (M8.5 wedge-hardening; issue #283).
//
// This gate ADVERSARIALLY VALIDATES the v1 trust posture — it probes the already-landed enforcement
// primitives as an attacker would, and each probe is a PERMANENT regression test. It rebuilds
// nothing: it drives the real bridge dispatcher scope gate (R-SEC-007), the real filesync path jail
// (R-SEC-008), the real importer isolation policy (R-SEC-006/008/010), and the real verify-before-use
// gate (R-SEC-009 / a08) from the outside, asserting each fails CLOSED.
//
// Ctest name: `redteam-trust-tier` — runs in the build job's general `ctest --preset dev` step on all
// three OS legs (not a milestone-gate prefix, so it is auto-picked with NO ci.yml change) and on the
// local dev gate. All in-process, GPU-free, no daemon, no secrets.
//
// The probes, one attacker viewpoint per section:
//   A  Dispatcher scope enforcement across every door (RPC/MCP-direct + the launch ceiling).
//   B  Path-jail traversal + narrowed-importer-jail escapes (R-SEC-008).
//   C  Scrubbed child env is an ALLOWLIST, never a passthrough (R-SEC-010).
//   D  Verify-before-use fails CLOSED against the pinned production trust root (R-SEC-009 / a08).
//
// Design refs: R-SEC-001/002/006/007/008/009/010/011(a), L-49, ROADMAP §1-M8.5.

#include "context/common/verify_signature.h"
#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/import/sandbox.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace rt
{
int g_failures = 0;
inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace rt

#define CHECK(cond)                                                                                    \
    do                                                                                                 \
    {                                                                                                  \
        if (!(cond))                                                                                   \
            ::rt::fail(__FILE__, __LINE__, #cond);                                                     \
    } while (false)

namespace bridge = context::editor::bridge;
namespace contract = context::editor::contract;
namespace filesync = context::editor::filesync;
namespace import = context::editor::import;
namespace common = context::common;

namespace
{

// The R-CLI-008 error code carried in a JSON-RPC error response's `error.data.code`, or "" when the
// response is not a scope-denied error. Mirrors the transport framing in dispatcher.cpp.
std::string wire_error_code(const std::string& response_json)
{
    contract::Json parsed = contract::Json::parse(response_json);
    if (!parsed.contains("error"))
        return {};
    const contract::Json& err = parsed.at("error");
    if (!err.contains("data") || !err.at("data").contains("code"))
        return {};
    return err.at("data").at("code").as_string();
}

// Attach a read/query session over the JSON-RPC wire (the door an MCP/RPC client uses), returning the
// mutated Session. The scope defaults to the read/query baseline — the least-privileged client.
bridge::Session attach_read_query_over_wire(const bridge::Dispatcher& d)
{
    bridge::Session session;
    const std::string attach =
        R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
        R"("capabilities":[],"scope":"read"}})";
    (void)d.handle(attach, session);
    return session;
}

// === Probe A — dispatcher scope enforcement across every door ======================================
// R-SEC-007: attach-token scopes are enforced in the RPC DISPATCHER (adapter-level tool filtering is
// bypassable via direct RPC), so a least-privileged (read/query) client hitting ANY out-of-scope verb
// — over the in-process dispatch() OR the JSON-RPC wire handle() the CLI/MCP doors both funnel through
// — is refused with the permission-class `scope.denied` code (exit-class 6, the consent_required class).
void probe_scope_enforcement_every_door()
{
    // The full out-of-scope surface a read/query attacker would rattle: the file-write family
    // (R-ARCH-002), the build/install family ("file-write is effectively code execution"), and the
    // session-lifecycle family. Every one must be refused for a read-only token.
    const std::vector<std::string> out_of_scope = {"set",           "new",
                                                   "edit",          "edit-batch",
                                                   "build",         "package.add",
                                                   "session.play",  "session.pause",
                                                   "session.step",  "shutdown"};

    // --- door 1: the in-process dispatch() gate -----------------------------------------------------
    {
        bridge::Dispatcher d;
        bridge::Session read;
        read.attached = true;
        read.scopes = bridge::ScopeSet::read_query();

        for (const std::string& method : out_of_scope)
        {
            const contract::Envelope env = d.dispatch(method, contract::Json::object(), read);
            CHECK(!env.ok());
            CHECK(env.error().has_value());
            CHECK(env.error()->code == bridge::kScopeDeniedCode); // "scope.denied"
            // Machine-readable: a scope-denied envelope classes as PERMISSION (exit code 6), so a
            // CLI/RPC/MCP caller branches on the consent class, not the generic error (1).
            CHECK(env.exit_code() == 6);
        }

        // Scope check PRECEDES verb resolution: a read token asking for `set` is told scope.denied,
        // NOT usage.unknown_verb / contract.unimplemented — it cannot even learn whether the gated
        // verb is wired (no surface enumeration for the under-scoped). This is the anti-recon property.
        const contract::Envelope probe = d.dispatch("set", contract::Json::object(), read);
        CHECK(probe.error()->code != "usage.unknown_verb");
        CHECK(probe.error()->code != "contract.unimplemented");

        // A read verb is allowed (baseline), proving the denials above are the SCOPE gate, not a
        // blanket refusal of everything.
        const contract::Envelope describe = d.dispatch("describe", contract::Json::object(), read);
        CHECK(describe.ok());
    }

    // --- door 2: the JSON-RPC wire handle() (what the CLI daemon client + the MCP adapter both use) --
    {
        bridge::Dispatcher d;
        bridge::Session session = attach_read_query_over_wire(d);
        CHECK(session.attached);

        for (const std::string& method : out_of_scope)
        {
            const std::string req = R"({"jsonrpc":"2.0","id":2,"method":")" + method +
                                    R"(","params":{}})";
            const std::string resp = d.handle(req, session);
            CHECK(wire_error_code(resp) == bridge::kScopeDeniedCode);
        }
    }

    // --- door 3: a privileged token reaches the backing (positive control — the gate is the ONLY
    //     thing stopping the read token; a fully-scoped token passes it) -----------------------------
    {
        bridge::Dispatcher d;
        bridge::Session privileged;
        privileged.attached = true;
        privileged.scopes = bridge::ScopeSet::all();
        const contract::Envelope env = d.dispatch("package.add", contract::Json::object(), privileged);
        CHECK(!env.ok());
        CHECK(env.error()->code == "contract.unimplemented"); // reserved backing, NOT scope.denied
    }
}

// The launch-ceiling clamp (R-SEC-007): a wire client CANNOT escalate past `--launch-scopes`. Even if
// it requests every scope in the attach handshake, the dispatcher intersects the request with the
// operator ceiling, so a read/query-ceilinged daemon grants read/query only — every write/build/session
// verb stays denied. This is the "attacker requests all, gets clamped to the floor" probe.
void probe_launch_ceiling_no_escalation()
{
    const bridge::Dispatcher d(nullptr, nullptr, bridge::ScopeSet::read_query()); // ceiling = read only

    contract::ClientHandshake client;
    client.protocol_major = contract::kProtocolMajor;
    // The attacker asks for the full privilege set.
    const bridge::Dispatcher::AttachResult result =
        d.attach(client, bridge::ScopeSet::parse("write build session"));
    CHECK(std::holds_alternative<bridge::Session>(result));
    const bridge::Session& s = std::get<bridge::Session>(result);

    // The granted scope set was clamped to the read/query baseline — no higher scope survived.
    CHECK(!s.scopes.has(bridge::Scope::file_write));
    CHECK(!s.scopes.has(bridge::Scope::session_control));
    CHECK(!s.scopes.has(bridge::Scope::build_install));
    const std::vector<std::string> names = s.scopes.names();
    CHECK(names.size() == 1);
    CHECK(names.front() == "read-query");

    // Consequently every privileged verb is still denied on the escalated-but-clamped token.
    CHECK(!bridge::authorize("set", s.scopes));
    CHECK(!bridge::authorize("build", s.scopes));
    CHECK(!bridge::authorize("package.add", s.scopes));
    CHECK(!bridge::authorize("session.play", s.scopes));
    // A dispatch on the clamped session confirms the wire refusal end-to-end.
    bridge::Session attacker = s;
    attacker.attached = true;
    const contract::Envelope env = d.dispatch("build", contract::Json::object(), attacker);
    CHECK(env.error()->code == bridge::kScopeDeniedCode);
    CHECK(env.exit_code() == 6);
}

// === Probe B — path-jail traversal + narrowed-importer-jail escapes (R-SEC-008) ====================
// The structural project-root jail must refuse every escape a crafted $ref / asset path / write target
// could smuggle. These are ADVERSARIAL inputs beyond the filesync unit test's set — deep/mixed
// traversal, prefix-adjacency, drive escapes — asserted against the real is_inside_jail primitive and
// the real narrowed importer read/write policy.
void probe_path_jail_traversal()
{
    // is_inside_jail: escapes are refused however they are spelled.
    CHECK(!filesync::is_inside_jail("proj", "proj/sub/../../../etc/passwd")); // deep net-escape
    CHECK(!filesync::is_inside_jail("proj", "proj/a/b/c/../../../../outside")); // over-deep ascent
    CHECK(!filesync::is_inside_jail("proj", "proj/./../proj-evil/secret"));    // prefix-adjacent sibling
    CHECK(!filesync::is_inside_jail("proj", "proj\\..\\etc\\passwd"));         // backslash traversal
    CHECK(!filesync::is_inside_jail("proj", "/etc/passwd"));                   // posix-absolute escape
    CHECK(!filesync::is_inside_jail("proj", "C:/Windows/system32"));           // windows-absolute escape
    // Positive controls: a path that dances through `..` but nets INSIDE stays admitted (an attacker
    // cannot turn a re-entering traversal into an escape; `proj/../proj` normalizes to the root itself).
    CHECK(filesync::is_inside_jail("proj", "proj/../proj"));                   // exits, re-enters the SAME root
    CHECK(filesync::is_inside_jail("proj", "proj/a/../b/c/../d.txt"));
    CHECK(filesync::is_inside_jail("proj", "proj/sub/../sub2/deep/x"));

    // The narrowed importer jail (owner ruling, issue #72): the effective read set is
    // (input-bytes ∪ declared-paths) ⊆ jail, and writes are confined to the output key. An attacker
    // controlling a declared-read path or a write target cannot use `..` (or a sibling prefix) to
    // widen past the jail.
    import::SandboxPolicy policy;
    policy.jail_root = "/project";
    policy.input_path = "/project/assets/hero.gltf";
    policy.output_key = "/project/.cache/gltf/mesh/abc";

    // A declared-read path that traverses OUT of the jail can never widen the read set.
    import::SandboxPolicy escaper = policy;
    escaper.declared_read_paths.push_back("/project/assets/../../etc"); // net-escapes /project
    CHECK(!import::read_permitted(escaper, "/etc/passwd"));
    CHECK(!import::read_permitted(escaper, "/project/assets/../../etc/passwd"));

    // A write target using `..` to climb out of the output key is refused; a sibling that merely SHARES
    // the key's textual prefix is NOT under it (prefix-adjacency), so it is refused too.
    CHECK(!import::write_permitted(policy, "/project/.cache/gltf/mesh/abc/../../../../etc/evil"));
    CHECK(!import::write_permitted(policy, "/project/.cache/gltf/mesh/abc-evil/blob")); // prefix-adjacent
    CHECK(!import::write_permitted(policy, "/project/assets/hero.gltf")); // in-jail, not the key
    // Positive controls: the input reads; the real output key (and its subtree) writes.
    CHECK(import::read_permitted(policy, "/project/assets/hero.gltf"));
    CHECK(import::write_permitted(policy, "/project/.cache/gltf/mesh/abc"));
    CHECK(import::write_permitted(policy, "/project/.cache/gltf/mesh/abc/blob"));
}

// === Probe C — the scrubbed child env is an ALLOWLIST, never a passthrough (R-SEC-010) =============
// The importer child inherits a deliberately-minimal, non-secret env built FROM SCRATCH — never the
// parent environment. The strong invariant (stronger than a substring denylist): every returned key is
// in a known-safe allowlist, so a future regression that lets an ambient secret/token/PATH through is
// caught structurally. And the default network posture is no-ambient-network.
void probe_scrubbed_env_is_allowlist()
{
    const std::vector<std::pair<std::string, std::string>> env = import::scrubbed_environment();

    // The only variables an importer child may see: a fixed, non-secret locale set (a determinism
    // aid). Anything outside this allowlist would be an ambient-inheritance regression.
    const std::vector<std::string> allowed = {"LANG", "LC_ALL", "LC_CTYPE", "TZ"};
    bool has_lang = false;
    for (const auto& [key, value] : env)
    {
        (void)value;
        bool in_allowlist = false;
        for (const std::string& ok : allowed)
            in_allowlist = in_allowlist || key == ok;
        CHECK(in_allowlist); // an ambient variable slipping through fails HERE
        if (key == "LANG")
            has_lang = true;

        // Belt-and-suspenders: no secret-shaped or ambient exfil-relevant name is ever present.
        CHECK(key.find("TOKEN") == std::string::npos);
        CHECK(key.find("SECRET") == std::string::npos);
        CHECK(key.find("KEY") == std::string::npos);
        CHECK(key.find("PASSWORD") == std::string::npos);
        CHECK(key != "PATH");
        CHECK(key != "HOME");
        CHECK(key != "USERPROFILE");
        CHECK(key != "GITHUB_TOKEN");
        CHECK(key != "AWS_SECRET_ACCESS_KEY");
        CHECK(key != "SSH_AUTH_SOCK");
    }
    CHECK(has_lang); // the C locale IS pinned (the allowlist is non-empty and deterministic)

    // No ambient network is the default (R-SEC-010): a fresh policy never allows egress.
    const import::SandboxPolicy fresh;
    CHECK(!fresh.allow_network);
}

// === Probe D — verify-before-use fails CLOSED against the pinned production root (R-SEC-009 / a08) ==
// The C++ half of the trust chain (the exact mirror of tools/verify_artifact.py). An attacker cannot
// get a non-production artifact past the PINNED PRODUCTION trust root, and the gate refuses on every
// abnormal path — a missing signature (unsigned ⇒ refused) and a missing/absent trust root (config
// refusal). None of these use implicit trust; verification is never "OK with a warning".
void probe_verify_before_use_fail_closed()
{
    const std::filesystem::path production_root = CONTEXT_TRUST_ROOT;         // the pinned in-repo root
    const std::filesystem::path fixtures = CONTEXT_VERIFY_FIXTURES_DIR;       // TEST-ONLY signer fixtures
    const std::filesystem::path artifact = fixtures / "sample-artifact.bin";
    const std::filesystem::path signature = fixtures / "sample-artifact.bin.sig";

    // (1) A TEST-key-signed artifact is REFUSED by the production root (which pins ONLY the production
    //     release key). Environment-independent: with ssh-keygen present this is a REFUSED verify; if
    //     ssh-keygen is somehow absent it is a CONFIG refusal — either way NEVER ok() (fail closed).
    const common::VerifyOutcome untrusted = common::verify_signature(
        artifact, signature, production_root, common::kReleaseSignerIdentity,
        common::kArtifactNamespace);
    CHECK(!untrusted.ok());

    // (2) An UNSIGNED artifact (missing detached signature) is the canonical fail-closed refusal —
    //     never used. Deterministic pre-check, no ssh-keygen needed.
    const common::VerifyOutcome unsigned_ = common::verify_signature(
        artifact, fixtures / "does-not-exist.sig", production_root);
    CHECK(!unsigned_.ok());
    CHECK(unsigned_.status == common::VerifyStatus::Refused);

    // (3) A MISSING / unreadable pinned trust root is a config-class refusal (no fallback to implicit
    //     trust — the non-TOFU root property). Still not ok().
    const common::VerifyOutcome no_root = common::verify_signature(
        artifact, signature, fixtures / "no" / "such" / "allowed_signers");
    CHECK(!no_root.ok());
    CHECK(no_root.status == common::VerifyStatus::ConfigError);

    // (4) A missing artifact is a config-class refusal (nothing to authenticate).
    const common::VerifyOutcome no_artifact =
        common::verify_signature(fixtures / "ghost.bin", signature, production_root);
    CHECK(!no_artifact.ok());
    CHECK(no_artifact.status == common::VerifyStatus::ConfigError);
}

} // namespace

int main()
{
    probe_scope_enforcement_every_door();
    probe_launch_ceiling_no_escalation();
    probe_path_jail_traversal();
    probe_scrubbed_env_is_allowlist();
    probe_verify_before_use_fail_closed();

    if (rt::g_failures != 0)
    {
        std::fprintf(stderr, "%d red-team check(s) FAILED\n", rt::g_failures);
        return 1;
    }
    std::printf("redteam-trust-tier: v1 trust posture held under adversarial probing "
                "(scope/path-jail/env-scrub/verify all fail-closed)\n");
    return 0;
}
