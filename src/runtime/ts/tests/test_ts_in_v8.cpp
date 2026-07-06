// R-QA-013 integration test (issue #83): authored TypeScript, transpiled + bundled by esbuild,
// RUNS in the task-2a in-process V8 host, exercising the host<->TS boundary BOTH ways. This is
// the DoD's headline proof — "authored TS runs in the shipped VM."
//
// It is CI-ONLY for its V8 dependency path (it links context_js, whose rusty_v8 prebuilt only
// links under the 3-OS CI build legs; the local Strawberry-GCC Windows dev gate builds the js
// STUB). So it mirrors test_js_engine.cpp's CONTEXT_JS_HAS_V8 split: the full flow on the CI
// legs, and on the local stub toolchain a reduced assertion (esbuild still transpiles; the V8
// backend correctly reports unavailable) so the local `ctest --preset dev` stays green.

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/ts_toolchain.h"

namespace tsv8
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace tsv8

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            tsv8::fail(__FILE__, __LINE__, #cond);                                                 \
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

// Bundle the authored gameplay entrypoint (game.ts) to a self-executing JS module.
std::string bundleGame(std::string& err)
{
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    if (!tc)
    {
        return {};
    }
    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Iife;
    cts::TranspileResult r = tc->transpile(kExamples + "/game.ts", opts);
    if (!r.ok)
    {
        err = r.diagnostics.empty() ? "bundle failed" : r.diagnostics.front().message;
        return {};
    }
    return r.js;
}

#ifdef CONTEXT_JS_HAS_V8
// The engine-provided host function game.ts calls at load time (JS -> host). Returns the seed
// plus a host-side bias read through `user`, proving a real value crosses INTO the host.
double hostBias(void* user, const double* args, std::size_t nargs)
{
    const double base = *static_cast<double*>(user);
    return base + (nargs > 0 ? args[0] : 0.0);
}
#endif
} // namespace

#ifdef CONTEXT_JS_HAS_V8

int main()
{
    // 1) authored TS -> JS (esbuild), locally verifiable but exercised here as the real input.
    std::string err;
    const std::string js = bundleGame(err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }

    // 2) run it in the task-2a V8 host.
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return 1;
    }

    // Bind the host function BEFORE eval — game.ts calls hostBias(7) at module top level.
    double bias = 100.0;
    CHECK(engine->bindHostFunction("hostBias", &hostBias, &bias, err));

    // 3) evaluate the bundle: runs hostBias(7) (JS -> host) and installs globalThis.update.
    CHECK(engine->eval(js, nullptr, err));

    // 4) host -> JS: resolve + call the exported gameplay entrypoint.
    //    update(x) = scale(x) + bias, bias = hostBias(7) = 100 + 7 = 107, scale(x) = x*3.
    cjs::FunctionHandle update = engine->getFunction("update");
    CHECK(update != cjs::kInvalidFunction);

    const double args10[1] = {10.0};
    double out = 0.0;
    CHECK(engine->callFunction(update, args10, 1, &out, err));
    CHECK(out == 137.0); // 10*3 + 107

    // update(0) = 0 + 107 — isolates the host-provided bias, proving the JS->host value flowed.
    const double args0[1] = {0.0};
    out = 0.0;
    CHECK(engine->callFunction(update, args0, 1, &out, err));
    CHECK(out == 107.0);

    if (tsv8::g_failures == 0)
    {
        std::printf("ts-in-v8: authored TS transpiled + ran in V8; host<->TS both ways OK\n");
    }
    return tsv8::g_failures == 0 ? 0 : 1;
}

#else // !CONTEXT_JS_HAS_V8 — local Strawberry-GCC dev gate (js stub). esbuild still works here.

int main()
{
    // The toolchain half IS locally exercisable even where V8 cannot link: prove authored TS
    // still bundles to JS, and that the V8 backend correctly reports unavailable (so the local
    // non-dependency gate is green; the CI build legs run the real in-V8 flow above).
    std::string err;
    const std::string js = bundleGame(err);
    CHECK(!js.empty());
    CHECK(js.find("globalThis.update") != std::string::npos);

    CHECK(!cjs::v8BackendAvailable());
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);

    if (tsv8::g_failures == 0)
    {
        std::printf("ts-in-v8: toolchain green; V8 backend not built on this toolchain (stub) — "
                    "CI build legs run the real in-V8 flow\n");
    }
    return tsv8::g_failures == 0 ? 0 : 1;
}

#endif // CONTEXT_JS_HAS_V8
