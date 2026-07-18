// Trust-tier ADVERSARIAL VALIDATION — `security-redteam-boundaries` (M8.5 Wave-1 a17, issue #283).
//
// This is a RED-TEAM suite: it probes the v1 trust posture as an attacker would, then stands as a
// permanent regression test so a future change that loosens a boundary reddens here. It does NOT
// rebuild the enforcement primitives (those are unit-tested per module); it cross-checks their SEAMS
// from the outside, the way a hostile CLI/RPC/MCP client, a malicious asset path, or a runaway agent
// would hit them. The doors, one per R-SEC control:
//
//   Door A  R-SEC-007  dispatcher scope gate — the scope check PRECEDES verb resolution (no
//                      MCP-adapter bypass: the dispatcher is the single enforcement point every door
//                      shares), the launch ceiling cannot be escalated over the wire, and the
//                      method->scope TABLE IS COMPLETE (every mutating verb is gated — a new mutating
//                      verb added without a scope mapping fails THIS test, not silently ship a bypass).
//   Door B  R-SEC-008  project-root path jail — traversal / absolute-reroot / drive-letter / mixed-
//                      separator escapes in a crafted $ref or asset path are refused, structurally.
//   Door C  R-SEC-006  importer read/write jail — an importer is confined to input-bytes ∪ declared
//          /R-SEC-008  paths (never the whole jail) for reads and to its own output key for writes;
//                      the declared-read escape hatch can never widen PAST the jail; fails closed.
//   Door D  R-SEC-010  scrubbed child environment — the importer/build/VM child env is an ALLOWLIST
//                      (locale only), never the parent env, so no ambient secret/token can exfiltrate.
//   Door E  R-SEC-006  per-OS sandbox-primitive HONESTY — os_sandbox_support() never claims a lockdown
//                      that is not there (Windows AppContainer is staged; enforced=false + a note).
//   Door F  R-SEC-001  in-process JS host — only ENGINE-INJECTED bindings are reachable; there is no
//          /R-SEC-002  ambient fs / net / process / require. Real on the 3-OS build legs (V8 linked);
//                      on the local Strawberry-GCC stub the factory must fail CLOSED (no half-real host).
//
// Findings (2026-07-17): every currently-WIRED boundary held — no exploitable bypass was found in the
// live surface. ONE defense-in-depth gap WAS found and fixed in this same lane: the R-SEC-007
// method->scope table gated only the currently-wired mutating verbs, leaving reserved-but-mutating
// verbs (install / migrate / merge-file / resolve-conflict / re-key / asset.move|rename /
// session.new|seed|inject|record / replay / ui.send) defaulting to the read/query baseline — so wiring
// any of their backings later would have silently exposed them to a read-only token. required_scope_for
// now gates them by semantic class (fail-closed). Door A's scope-table sweep is that fix's regression
// test. Residual v1 boundaries are catalogued in docs/security-boundaries-v1.md.
//
// All in-process, GPU-free, no daemon, no network — runs in the general ctest step on every build leg.

#include "m8_exit_test.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/import/sandbox.h"
#include "context/runtime/js/js_host.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace bridge = context::editor::bridge;
namespace contract = context::editor::contract;
namespace filesync = context::editor::filesync;
namespace import = context::editor::import;
namespace js = context::runtime::js;

using bridge::Scope;
using bridge::ScopeSet;
using bridge::Session;
using context::tests::m8::report;

namespace
{

// A read/query-only attached session — the unrecognized-client / read-only-reviewer baseline.
Session read_only_session()
{
    Session s;
    s.attached = true;
    s.scopes = ScopeSet::read_query();
    return s;
}

// A minimal HostFunction: the ONE capability the engine injects into the JS host (Door F).
double injected_binding(void*, const double*, std::size_t)
{
    return 42.0;
}

} // namespace

int main()
{
    // ================================================================================================
    // Door A — R-SEC-007: the dispatcher scope gate is the SINGLE enforcement point, precedes verb
    //          resolution, cannot be escalated over the wire, and its method->scope table is COMPLETE.
    // ================================================================================================
    {
        bridge::Dispatcher d; // default ceiling = all scopes; no stream, no backend
        const Session read = read_only_session();

        // (A1) Every mutating method is refused BEFORE the verb resolves. `scope.denied` — NOT
        // `contract.unimplemented`, NOT `usage.unknown_verb`: an under-scoped token cannot even learn
        // whether a verb is implemented (the exact door a direct-RPC client uses to bypass the MCP
        // adapter's tool filtering; enforcement lives HERE, so there is nothing to bypass). This sweeps
        // the newly-gated reserved verbs too, proving the scope check fires ahead of their reserved
        // contract.unimplemented backing.
        for (const char* method : {"package.add", "build", "install", "set", "new", "edit",
                                   "edit-batch", "migrate", "merge-file", "resolve-conflict", "re-key",
                                   "asset.move", "asset.rename", "session.new", "session.step",
                                   "session.seed", "session.inject", "session.record", "replay",
                                   "ui.send", "shutdown"})
        {
            const contract::Envelope e = d.dispatch(method, contract::Json::object(), read);
            CHECK(!e.ok());
            CHECK(e.error().has_value() && e.error()->code == bridge::kScopeDeniedCode);
        }

        // (A2) A read verb still resolves under the read baseline (the gate is least-privilege, not a
        // blanket denial) — describe returns the self-description.
        const contract::Envelope descr = d.dispatch("describe", contract::Json::object(), read);
        CHECK(descr.ok());

        // (A3) The launch-time ceiling clamps a WIRE client's requested scopes and cannot be escalated:
        // a dispatcher launched read-only intersects an attach that ASKS for every scope down to
        // read/query, so the identical mutating call is refused on the clamped session too.
        bridge::Dispatcher clamped(nullptr, nullptr, ScopeSet::read_query());
        contract::ClientHandshake client;
        client.protocol_major = contract::kProtocolMajor;
        const bridge::Dispatcher::AttachResult attached =
            clamped.attach(client, ScopeSet::all()); // request EVERYTHING
        const Session* got = std::get_if<Session>(&attached);
        CHECK(got != nullptr);
        if (got != nullptr)
        {
            CHECK(got->scopes.has(Scope::read_query));          // baseline survives
            CHECK(!got->scopes.has(Scope::file_write));         // escalation refused
            CHECK(!got->scopes.has(Scope::session_control));    // "
            CHECK(!got->scopes.has(Scope::build_install));      // "
            const contract::Envelope w = clamped.dispatch("edit", contract::Json::object(), *got);
            CHECK(!w.ok());
            CHECK(w.error().has_value() && w.error()->code == bridge::kScopeDeniedCode);
        }

        // (A4) intersect() is monotone — clamping can only SHRINK a scope set, never grant one. A read
        // ceiling ∩ all == read; an all ceiling ∩ read == read. No composition of clamps yields a scope
        // neither side held.
        CHECK(!ScopeSet::read_query().intersect(ScopeSet::all()).has(Scope::build_install));
        CHECK(!ScopeSet::all().intersect(ScopeSet::read_query()).has(Scope::file_write));

        // (A5) SCOPE-TABLE COMPLETENESS TRIPWIRE (the fix's regression test). Every verb in the LIVE
        // contract registry is classified here by its semantic scope. A mutating verb that defaults to
        // read/query is a fail-open; a NEW verb added to the registry without a line here FAILS this
        // test ("unclassified verb") — forcing its author to classify the scope BEFORE it can ship. So
        // the enforcement point (required_scope_for) can never silently drift open under a read token.
        const std::map<std::string, Scope> expected = {
            // read/query baseline — reads, observability, and the derived-index refresh
            {"describe", Scope::read_query},        {"query", Scope::read_query},
            {"snapshot", Scope::read_query},        {"subscribe", Scope::read_query},
            {"unsubscribe", Scope::read_query},     {"ack", Scope::read_query},
            {"resource.read", Scope::read_query},   {"validate", Scope::read_query},
            {"doctor", Scope::read_query},          {"reconcile", Scope::read_query},
            {"session.hash", Scope::read_query},    {"determinism.diff", Scope::read_query},
            {"profile.gc", Scope::read_query},      {"profile.session", Scope::read_query},
            {"debug.attach", Scope::read_query},
            {"ui.dump", Scope::read_query},         {"ui.query", Scope::read_query},
            {"ui.assert", Scope::read_query},
            // file_write — authored-file mutations
            {"new", Scope::file_write},             {"set", Scope::file_write},
            {"edit", Scope::file_write},            {"edit-batch", Scope::file_write},
            {"migrate", Scope::file_write},         {"merge-file", Scope::file_write},
            {"resolve-conflict", Scope::file_write},{"re-key", Scope::file_write},
            {"asset.move", Scope::file_write},      {"asset.rename", Scope::file_write},
            // build_install — install / build (code execution)
            {"package.add", Scope::build_install},  {"install", Scope::build_install},
            {"build", Scope::build_install},
            // session_control — create/drive a running session
            {"session.new", Scope::session_control},   {"session.step", Scope::session_control},
            {"session.seed", Scope::session_control},  {"session.inject", Scope::session_control},
            {"session.record", Scope::session_control},{"replay", Scope::session_control},
            {"ui.send", Scope::session_control},        {"shutdown", Scope::session_control},
        };
        for (const contract::VerbSpec& v : contract::Registry::instance().verbs())
        {
            const auto it = expected.find(v.rpc_method);
            // A registry verb with no classification here is a NEW verb whose scope was never decided.
            CHECK(it != expected.end());
            if (it == expected.end())
            {
                std::fprintf(stderr, "  unclassified registry verb (add it to the red-team scope "
                                     "table): %s\n",
                             v.rpc_method.c_str());
                continue;
            }
            // The live enforcement point must agree with the semantic classification.
            CHECK(bridge::required_scope_for(v.rpc_method) == it->second);
            // Cross-check via authorize(): a read/query token may call ONLY the read-baseline verbs.
            const bool read_reachable = bridge::authorize(v.rpc_method, ScopeSet::read_query());
            CHECK(read_reachable == (it->second == Scope::read_query));
            // A fully-scoped operator token can reach EVERY verb (no verb is un-authorizable / dead).
            CHECK(bridge::authorize(v.rpc_method, ScopeSet::all()));
        }
    }

    // ================================================================================================
    // Door B — R-SEC-008: the project-root path jail refuses every traversal an attacker-controlled
    //          $ref / asset path could carry. Structural (normalize + containment), not string-match.
    // ================================================================================================
    {
        const std::string root = "project";

        // Legitimate in-jail paths pass.
        CHECK(filesync::is_inside_jail(root, "project/scenes/main.scene.json"));
        CHECK(filesync::is_inside_jail(root, "project")); // the root itself
        CHECK(filesync::is_inside_jail(root, "project/a/b/../b/c")); // normalizes back inside

        // Every escape vector is refused.
        CHECK(!filesync::is_inside_jail(root, "project/../secret"));        // climb out one level
        CHECK(!filesync::is_inside_jail(root, "project/../../etc/passwd")); // climb out several
        CHECK(!filesync::is_inside_jail(root, "../project"));               // sibling of the root
        CHECK(!filesync::is_inside_jail(root, "/etc/passwd"));              // absolute POSIX reroot
        CHECK(!filesync::is_inside_jail(root, "C:/Windows/System32"));      // absolute drive reroot
        CHECK(!filesync::is_inside_jail(root, "project\\..\\secret"));      // backslash traversal
        CHECK(!filesync::is_inside_jail(root, "projectile/x")); // prefix-not-a-parent (the classic
                                                                // "startsWith" jail bug)
        CHECK(!filesync::is_inside_jail(root, "project/../project-evil/x")); // sibling via climb

        // normalize_path collapses the tricks a jail bypass would rely on.
        CHECK(filesync::normalize_path("a/./b/../c") == "a/c");
        CHECK(filesync::normalize_path("a\\b\\..\\c") == "a/c"); // backslash unified then folded
        CHECK(filesync::normalize_path("/x/../../y") == "/y");   // cannot climb past an absolute root
    }

    // ================================================================================================
    // Door C — R-SEC-006 / R-SEC-008: the importer read/write jail. An importer sees input-bytes ∪
    //          declared paths for reads and ONLY its own output key for writes — never the whole jail,
    //          never outside it. The declared-read escape hatch can never widen PAST the jail.
    // ================================================================================================
    {
        import::SandboxPolicy pol;
        pol.jail_root = "proj";
        pol.input_path = "proj/assets/model.gltf";
        pol.output_key = "proj/.cache/ab12";
        pol.declared_read_paths = {"proj/assets/model.bin"}; // a legit sibling the glTF references

        // Reads: the source + the declared sibling are permitted; an unrelated in-jail file is NOT
        // (input-bytes-only, NOT jail-wide — the owner ruling on issue #72).
        CHECK(import::read_permitted(pol, "proj/assets/model.gltf"));
        CHECK(import::read_permitted(pol, "proj/assets/model.bin"));
        CHECK(!import::read_permitted(pol, "proj/secrets/private.key")); // in jail, but not granted
        CHECK(!import::read_permitted(pol, "proj/scenes/other.scene.json"));

        // Traversal out of the jail via the input/declared paths is refused (outer bound wins).
        CHECK(!import::read_permitted(pol, "proj/../etc/passwd"));
        CHECK(!import::read_permitted(pol, "/etc/passwd"));

        // The declared-read escape hatch can NEVER widen past the jail: a declared path that resolves
        // outside jail_root grants nothing.
        import::SandboxPolicy widen = pol;
        widen.declared_read_paths = {"proj/../outside"}; // attacker-declared jailbreak
        CHECK(!import::read_permitted(widen, "proj/../outside/loot"));
        CHECK(!import::read_permitted(widen, "outside/loot"));

        // Writes: ONLY the importer's own output key (and paths beneath it) — never elsewhere in the
        // jail, never outside it.
        CHECK(import::write_permitted(pol, "proj/.cache/ab12"));
        CHECK(import::write_permitted(pol, "proj/.cache/ab12/tex0.ktx2"));
        CHECK(!import::write_permitted(pol, "proj/.cache/other")); // a different key
        CHECK(!import::write_permitted(pol, "proj/assets/model.gltf")); // cannot overwrite its input
        CHECK(!import::write_permitted(pol, "proj/../loot"));           // cannot escape the jail

        // Fail closed on a malformed / empty policy — no read, no write is permitted by default.
        const import::SandboxPolicy empty;
        CHECK(!import::read_permitted(empty, "anything"));
        CHECK(!import::write_permitted(empty, "anything"));
    }

    // ================================================================================================
    // Door D — R-SEC-010: the scrubbed child environment is an ALLOWLIST, never the parent env, so an
    //          importer/build/VM child cannot inherit — and exfiltrate — an ambient secret or token.
    // ================================================================================================
    {
        const std::vector<std::pair<std::string, std::string>> env = import::scrubbed_environment();

        // It is exactly the deterministic locale allowlist — nothing else crosses.
        CHECK(env.size() == 2);
        bool has_lang = false, has_lc_all = false;
        for (const auto& [k, v] : env)
        {
            if (k == "LANG" && v == "C")
                has_lang = true;
            if (k == "LC_ALL" && v == "C")
                has_lc_all = true;
        }
        CHECK(has_lang);
        CHECK(has_lc_all);

        // None of the common secret/credential/ambient variables an attacker would harvest are present
        // — the child env is NOT the parent env (R-SEC-003 reinforced by R-SEC-010).
        for (const char* leaky : {"AWS_SECRET_ACCESS_KEY", "AWS_ACCESS_KEY_ID", "GITHUB_TOKEN",
                                  "GH_TOKEN", "NPM_TOKEN", "SSH_AUTH_SOCK", "HOME", "USERPROFILE",
                                  "PATH", "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "SECRET", "TOKEN",
                                  "PASSWORD"})
        {
            for (const auto& [k, v] : env)
            {
                (void)v;
                CHECK(k != leaky);
            }
        }
    }

    // ================================================================================================
    // Door E — R-SEC-006: honest per-OS sandbox-primitive staging. The runner never CLAIMS a lockdown
    //          that is not there — where a primitive is not enforced it says so, with a tracked note.
    // ================================================================================================
    {
        const import::OsSandboxSupport sup = import::os_sandbox_support();
        CHECK(!sup.primitive.empty());
        // The invariant: enforced == (there is no follow-up note). An unenforced platform (Windows
        // AppContainer, still staged) MUST carry a non-empty follow-up, never silently report enforced.
        if (sup.enforced)
            CHECK(sup.follow_up.empty());
        else
            CHECK(!sup.follow_up.empty());
#if defined(_WIN32)
        // On this Windows executor the syscall-sandbox lockdown is a documented residual — enforced is
        // false and the portable in-process slice (jail + scrubbed env + no network) is the fallback.
        CHECK(!sup.enforced);
        CHECK(sup.primitive == "windows-appcontainer");
        const import::SandboxApplyResult applied = import::apply_importer_sandbox();
        CHECK(!applied.applied); // honestly not applied here — never a false "locked down"
#elif defined(__linux__) || defined(__APPLE__)
        CHECK(sup.enforced); // seccomp-bpf (Linux) / Seatbelt (macOS) are enforced now
#endif
    }

    // ================================================================================================
    // Door F — R-SEC-001 / R-SEC-002: the in-process JS host exposes ONLY engine-injected bindings —
    //          no ambient fs / net / process / require. REAL on the 3-OS build legs (V8 linked); on the
    //          local Strawberry-GCC stub the factory must fail CLOSED (never a half-real, unsandboxed host).
    // ================================================================================================
    {
        if (js::v8BackendAvailable())
        {
            std::string err;
            std::unique_ptr<js::JsEngine> engine = js::createV8Engine(err);
            CHECK(engine != nullptr);
            if (engine != nullptr)
            {
                // The ONE capability the engine grants is an injected binding — it IS reachable.
                CHECK(engine->bindHostFunction("__ctx_injected", &injected_binding, nullptr, err));
                double n = -1.0;
                CHECK(engine->eval("typeof __ctx_injected === 'function' ? 1 : 0", &n, err));
                CHECK(n == 1.0);

                // NO ambient capability is reachable: fs / net / process / require / module and the
                // usual host-escape globals are all undefined (constrained host ABI — R-SEC-001).
                for (const char* ambient : {"process", "require", "module", "global", "Deno",
                                            "XMLHttpRequest", "fetch", "WebAssembly", "eval",
                                            "Function"})
                {
                    // NB: `eval`/`Function`/`WebAssembly` ARE standard ECMAScript globals; asserting
                    // they are absent would be wrong. We only assert the CAPABILITY escapes
                    // (process/require/fs/net) are undefined — the language builtins are not a host
                    // capability. Skip these three builtins.
                    const std::string name(ambient);
                    if (name == "eval" || name == "Function" || name == "WebAssembly")
                        continue;
                    double u = -1.0;
                    const std::string probe = "typeof " + name + " === 'undefined' ? 1 : 0";
                    CHECK(engine->eval(probe, &u, err));
                    CHECK(u == 1.0);
                }
                // No ambient module loader / filesystem object on globalThis either.
                double g = -1.0;
                CHECK(engine->eval("typeof globalThis.fs === 'undefined' ? 1 : 0", &g, err));
                CHECK(g == 1.0);
            }
        }
        else
        {
            // Stub build (the local Strawberry-GCC dev host cannot link the MSVC/Clang-ABI V8 prebuilt).
            // The factory MUST fail closed: no engine + a reason. A stub that silently "worked" would be
            // an unsandboxed JS host — the exact bypass this door guards against.
            std::string err;
            std::unique_ptr<js::JsEngine> engine = js::createV8Engine(err);
            CHECK(engine == nullptr);
            CHECK(!err.empty());
        }
    }

    return report("security-redteam-boundaries",
                  "trust-tier adversarial validation — scope gate (R-SEC-007) precedes resolution + "
                  "complete table, path jail (R-SEC-008), importer read/write jail + scrubbed env "
                  "(R-SEC-006/010), sandbox-staging honesty, and JS injected-bindings-only "
                  "(R-SEC-001/002) all held / fail closed");
}
