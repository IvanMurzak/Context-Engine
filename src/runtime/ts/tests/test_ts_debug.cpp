// R-QA-013 headline test for the R-OBS-005 INTERACTIVE half (issue #94): a breakpoint set in
// authored TypeScript resolves — through the 2b-i-emitted Source Map v3 — to the running transpiled
// JS, and V8's in-box CDP inspector PAUSES there; the pump then resumes and the program completes.
// This is the "attach a CDP client, hit a source-mapped breakpoint, resume/step" DoD proof, driven
// programmatically over real CDP messages (the exact protocol Chrome DevTools / VS Code speak).
//
// CI-ONLY for its V8 dependency path (it links context_js, whose rusty_v8 prebuilt links only on the
// 3-OS CI build legs). It mirrors test_ts_in_v8.cpp's CONTEXT_JS_HAS_V8 split: the full attach +
// breakpoint + resume flow on the CI legs; on the local Strawberry-GCC Windows dev gate (js stub)
// the SOURCE-MAP HALF is still exercised locally (esbuild transpiles, the map parses, and the
// authored-TS -> generated-JS forward resolution the breakpoint relies on round-trips), and the V8
// backend correctly reports unavailable — so the local `ctest --preset dev` stays green.

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/source_map.h"
#include "context/runtime/ts/ts_toolchain.h"

namespace dbgtest
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace dbgtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            dbgtest::fail(__FILE__, __LINE__, #cond);                                              \
    } while (false)

namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_TS_EXAMPLES_DIR
#error "CONTEXT_TS_EXAMPLES_DIR must be defined by CMake (the authored .ts examples dir)"
#endif

namespace
{
const std::string kEsbuild = CONTEXT_ESBUILD_PATH;
const std::string kExamples = CONTEXT_TS_EXAMPLES_DIR;

// Bundle breakpoint.ts to a self-executing JS module WITH a Source Map v3. Returns the JS and, via
// `mapOut`, the map JSON.
std::string bundleBreakpoint(std::string& err, std::string& mapOut)
{
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    if (!tc)
    {
        return {};
    }
    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Iife;
    opts.sourcemap = true;
    cts::TranspileResult r = tc->transpile(kExamples + "/breakpoint.ts", opts);
    if (!r.ok)
    {
        err = r.diagnostics.empty() ? "bundle failed" : r.diagnostics.front().message;
        return {};
    }
    mapOut = r.sourceMap;
    return r.js;
}

// The 0-based authored line of the breakpoint target in breakpoint.ts — the line carrying "n * 2".
// Derived from the fixture text so the test does not hardcode a line number.
std::optional<std::uint32_t> breakpointLine()
{
    std::ifstream in(kExamples + "/breakpoint.ts", std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::string line;
    std::uint32_t idx = 0;
    while (std::getline(in, line))
    {
        if (line.find("n * 2") != std::string::npos)
        {
            return idx;
        }
        ++idx;
    }
    return std::nullopt;
}

// Used only in the CONTEXT_JS_HAS_V8 (CI) branch; the local stub build compiles it out of use.
[[maybe_unused]] bool anyContains(const std::vector<std::string>& msgs, const char* needle)
{
    for (const std::string& m : msgs)
    {
        if (m.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

// The shared source-map half, exercised on EVERY toolchain (no V8): the authored breakpoint line
// forward-resolves to a generated JS position, which reverse-resolves back to breakpoint.ts —
// proving the mapping the interactive breakpoint depends on round-trips on a REAL esbuild map.
// Returns the generated position for the CI leg to set the breakpoint at (via `genOut`).
bool checkSourceMapHalf(const cts::SourceMap& map, cts::GeneratedPosition& genOut)
{
    const std::optional<std::uint32_t> tsLine = breakpointLine();
    CHECK(tsLine.has_value());
    if (!tsLine.has_value())
    {
        return false;
    }
    // Forward: authored breakpoint.ts (line, col 0) -> generated JS position.
    std::optional<cts::GeneratedPosition> gen = map.resolveGenerated("breakpoint.ts", *tsLine, 0);
    CHECK(gen.has_value());
    if (!gen.has_value())
    {
        return false;
    }
    // Reverse: that generated position maps back onto breakpoint.ts (the source-map round-trip).
    std::optional<cts::OriginalPosition> back = map.resolve(gen->line, gen->column);
    CHECK(back.has_value());
    if (back.has_value())
    {
        CHECK(back->source.find("breakpoint.ts") != std::string::npos);
    }
    genOut = *gen;
    return true;
}
} // namespace

#ifdef CONTEXT_JS_HAS_V8

int main()
{
    std::string err;
    std::string mapJson;
    const std::string js = bundleBreakpoint(err, mapJson);
    CHECK(!js.empty());
    CHECK(!mapJson.empty());
    if (js.empty() || mapJson.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }

    std::optional<cts::SourceMap> map = cts::SourceMap::parse(mapJson, &err);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return 1;
    }
    cts::GeneratedPosition gen{};
    if (!checkSourceMapHalf(*map, gen))
    {
        return 1;
    }

    // --- attach a CDP session over the V8 in-box inspector ---------------------------------------
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return 1;
    }
    std::unique_ptr<cjs::InspectorSession> session = engine->attachInspector(err);
    CHECK(session != nullptr);
    if (!session)
    {
        std::fprintf(stderr, "attachInspector: %s\n", err.c_str());
        return 1;
    }

    std::vector<std::string> msgs;
    session->onMessage([&msgs](std::string_view m) { msgs.emplace_back(m); });

    // On pause, resume once (a real client would step; resume proves the breakpoint was hit and the
    // program can be driven to completion over CDP).
    bool paused_seen = false;
    session->onPause([&]() -> bool {
        paused_seen = true;
        std::string e;
        session->dispatch(R"({"id":900,"method":"Debugger.resume"})", e);
        return true;
    });

    const char* kUrl = "context://breakpoint.js";
    CHECK(session->dispatch(R"({"id":1,"method":"Runtime.enable"})", err));
    CHECK(session->dispatch(R"({"id":2,"method":"Debugger.enable"})", err));

    // Set the source-mapped breakpoint: authored breakpoint.ts -> generated (gen.line, gen.column),
    // targeting the URL the script runs under. setBreakpointByUrl accepts a pending breakpoint before
    // the script parses and binds it on scriptParsed (the standard client flow).
    const std::string setBp = std::string(R"({"id":3,"method":"Debugger.setBreakpointByUrl",)") +
                              R"("params":{"url":")" + kUrl +
                              R"(","lineNumber":)" + std::to_string(gen.line) +
                              R"(,"columnNumber":)" + std::to_string(gen.column) + "}}";
    CHECK(session->dispatch(setBp, err));
    // The breakpoint bound (V8 returns a breakpointId; a non-empty locations array means it resolved
    // to a real location in the parsed script).
    CHECK(anyContains(msgs, "breakpointId"));

    // Run the bundle under the breakpoint URL; step(20) executes the marked line and pauses there.
    CHECK(session->run(js, kUrl, err));
    CHECK(paused_seen);
    CHECK(anyContains(msgs, "Debugger.paused"));

    // The program ran to completion after resume: the module-load call landed its result.
    double result = 0.0;
    CHECK(engine->eval("globalThis.__bp_result", &result, err));
    CHECK(result == 41.0); // step(20) = 20*2 + 1

    session.reset();
    if (dbgtest::g_failures == 0)
    {
        std::printf("ts-debug: CDP attach + source-mapped breakpoint hit in authored TS + resume "
                    "OK\n");
    }
    return dbgtest::g_failures == 0 ? 0 : 1;
}

#else // !CONTEXT_JS_HAS_V8 — local Strawberry-GCC dev gate (js stub). esbuild + source_map work here.

int main()
{
    // The source-map half is fully local: breakpoint.ts bundles WITH a map, the map parses, and the
    // authored-TS -> generated-JS forward resolution the breakpoint relies on round-trips. The live
    // V8 attach + pause is the CI-only leg above.
    std::string err;
    std::string mapJson;
    const std::string js = bundleBreakpoint(err, mapJson);
    CHECK(!js.empty());
    CHECK(!mapJson.empty());

    std::optional<cts::SourceMap> map = cts::SourceMap::parse(mapJson, &err);
    CHECK(map.has_value());
    if (map.has_value())
    {
        cts::GeneratedPosition gen{};
        checkSourceMapHalf(*map, gen);
    }

    // The V8 backend is not built on this toolchain.
    CHECK(!cjs::v8BackendAvailable());
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);

    if (dbgtest::g_failures == 0)
    {
        std::printf("ts-debug: source-map breakpoint round-trip green; V8 attach is CI-only "
                    "(stub)\n");
    }
    return dbgtest::g_failures == 0 ? 0 : 1;
}

#endif // CONTEXT_JS_HAS_V8
