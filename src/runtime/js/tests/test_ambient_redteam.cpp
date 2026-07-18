// a17 — TRUST-TIER RED-TEAM: the in-process JS host exposes NO ambient fs/net/process (issue #283).
//
// R-SEC-001/002 (L-49): WASM/TypeScript packages run sandboxed; gameplay TS reaches ONLY the bindings
// the host explicitly injects. This probe attacks that claim from inside a guest script: it asserts the
// V8 host globalThis carries none of the ambient capabilities a Node/Deno/browser runtime would hand a
// script (a module loader → fs, a process object → env/fs/net, fetch/XHR/WebSocket → net, a foreign
// runtime object), and — the positive control — that a host-bound function IS reachable, proving the
// ONLY door into the host is an explicit injection (bindHostFunction), never an ambient global.
//
// Ctest name: `redteam-js-ambient`. V8-GATED like the whole js test family: the real assertions run
// wherever the rusty_v8 backend links (the CI legs / a Clang/MSVC host with the prebuilt); on a
// toolchain that only builds the stub (the local Strawberry-GCC dev gate) createV8Engine() is
// unavailable, so the probe asserts the honest stub state (no JS runs at all ⇒ no ambient surface).
// This mirrors test_js_engine.cpp's CONTEXT_JS_HAS_V8 / v8BackendAvailable() structure exactly.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "context/runtime/js/js_host.h"

namespace jstest
{
int g_failures = 0;
inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace jstest

#define CHECK(cond)                                                                                    \
    do                                                                                                 \
    {                                                                                                  \
        if (!(cond))                                                                                   \
            jstest::fail(__FILE__, __LINE__, #cond);                                                   \
    } while (false)

namespace cjs = context::runtime::js;

#ifdef CONTEXT_JS_HAS_V8

// A host callback the probe binds to prove that an INJECTED binding is the only way into the host: it
// returns its single argument doubled, so the guest can observe a real host round-trip.
static double redteamHostEcho(void* /*user*/, const double* args, std::size_t nargs)
{
    return nargs >= 1 ? args[0] * 2.0 : -1.0;
}

// Evaluate `expr` (which must yield 1 for "absent"/pass, 0 for "present"/fail) and CHECK it is 1.
// `label` names the ambient global under test for a legible failure.
static void assert_absent(cjs::JsEngine& engine, const char* label, const std::string& expr)
{
    double result = -1.0;
    std::string err;
    const bool ok = engine.eval(expr, &result, err);
    CHECK(ok);
    if (!ok)
        std::fprintf(stderr, "  eval error probing %s: %s\n", label, err.c_str());
    CHECK(result == 1.0);
    if (result != 1.0)
        std::fprintf(stderr, "  AMBIENT CAPABILITY REACHABLE: %s is defined in the JS host\n", label);
}

int main()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    // The V8 backend is compiled in, so the host MUST instantiate here (unlike the stub build).
    CHECK(engine != nullptr);
    if (!engine)
    {
        std::fprintf(stderr, "createV8Engine failed with V8 compiled in: %s\n", err.c_str());
        return 1;
    }

    // --- no ambient MODULE LOADER (→ fs): a guest cannot require()/module its way to the filesystem.
    assert_absent(*engine, "require", "(typeof require === 'undefined') ? 1 : 0");
    assert_absent(*engine, "module", "(typeof module === 'undefined') ? 1 : 0");
    assert_absent(*engine, "exports", "(typeof exports === 'undefined') ? 1 : 0");
    assert_absent(*engine, "__dirname", "(typeof __dirname === 'undefined') ? 1 : 0");
    assert_absent(*engine, "__filename", "(typeof __filename === 'undefined') ? 1 : 0");
    assert_absent(*engine, "importScripts", "(typeof importScripts === 'undefined') ? 1 : 0");

    // --- no ambient PROCESS object (→ env / fs / net / child_process).
    assert_absent(*engine, "process", "(typeof process === 'undefined') ? 1 : 0");
    assert_absent(*engine, "global", "(typeof global === 'undefined') ? 1 : 0"); // Node's global alias

    // --- no ambient NETWORK surface.
    assert_absent(*engine, "fetch", "(typeof fetch === 'undefined') ? 1 : 0");
    assert_absent(*engine, "XMLHttpRequest", "(typeof XMLHttpRequest === 'undefined') ? 1 : 0");
    assert_absent(*engine, "WebSocket", "(typeof WebSocket === 'undefined') ? 1 : 0");

    // --- no foreign-runtime capability objects (they bundle fs + net).
    assert_absent(*engine, "Deno", "(typeof Deno === 'undefined') ? 1 : 0");
    assert_absent(*engine, "Bun", "(typeof Bun === 'undefined') ? 1 : 0");

    // --- globalThis carries no injected fs handle by default.
    assert_absent(*engine, "globalThis.fs",
                  "(typeof globalThis.fs === 'undefined' && typeof globalThis.require === 'undefined') "
                  "? 1 : 0");

    // --- POSITIVE CONTROL: an EXPLICITLY INJECTED host binding IS reachable, proving injection is the
    //     sole door in (a guest reaches the host only through what the embedder chose to expose). Before
    //     binding, the name is absent; after binding, calling it round-trips through real host code.
    assert_absent(*engine, "redteamHostEcho(pre-bind)",
                  "(typeof redteamHostEcho === 'undefined') ? 1 : 0");
    CHECK(engine->bindHostFunction("redteamHostEcho", &redteamHostEcho, nullptr, err));
    double round = 0.0;
    CHECK(engine->eval("redteamHostEcho(21)", &round, err));
    CHECK(round == 42.0); // the injected binding, and ONLY it, crosses into host code

    if (jstest::g_failures != 0)
    {
        std::fprintf(stderr, "%d red-team check(s) FAILED\n", jstest::g_failures);
        return 1;
    }
    std::printf("redteam-js-ambient: JS host exposes only injected bindings "
                "(no ambient fs/net/process)\n");
    return 0;
}

#else // !CONTEXT_JS_HAS_V8 — the stub build (local Strawberry-GCC dev gate).

int main()
{
    // No V8 backend on this toolchain: the host cannot instantiate, so NO guest JS runs at all and
    // there is no ambient surface to reach. Assert the honest stub state (mirrors test_js_engine.cpp),
    // keeping the local non-dependency gate green; the real ambient probes run on the V8 CI legs.
    std::string err;
    CHECK(!cjs::v8BackendAvailable());
    CHECK(cjs::createV8Engine(err) == nullptr);

    if (jstest::g_failures != 0)
    {
        std::fprintf(stderr, "%d red-team check(s) FAILED\n", jstest::g_failures);
        return 1;
    }
    std::printf("redteam-js-ambient: V8 backend absent (stub) — ambient probes are V8-CI-gated\n");
    return 0;
}

#endif // CONTEXT_JS_HAS_V8
