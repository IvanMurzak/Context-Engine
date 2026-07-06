// R-QA-013 tests for the esbuild TypeScript toolchain (issue #83). Zero-dependency harness (the
// repo carries no C++ test framework — each test is a plain executable that CHECK()s its
// invariants and returns non-zero on any failure), mirroring runtime/js/tests/test_js_engine.cpp.
//
// This is a LOCAL gate (unlike runtime/js): esbuild is a native binary invoked as a subprocess,
// so it builds + runs under the local Strawberry-GCC Windows dev gate. It covers the toolchain in
// isolation — availability, version, transpile-strips-types, bundle-inlines-imports, and the two
// error classes (ts.transpile_failed / ts.bundle_failed) surfaced through the R-CLI-008 codes.
// The separate test_ts_in_v8.cpp (CI-only) proves the emitted JS actually runs in the V8 host.

#include <cstdio>
#include <memory>
#include <string>

#include "context/runtime/ts/ts_toolchain.h"

namespace tstest
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace tstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            tstest::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

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

std::string example(const char* name) { return kExamples + "/" + name; }

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}
} // namespace

static void test_available_and_version()
{
    CHECK(cts::esbuildToolchainAvailable(kEsbuild));
    CHECK(!cts::esbuildToolchainAvailable(kEsbuild + "-does-not-exist"));

    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    CHECK(tc->name() == "esbuild");
    err.clear();
    const std::string v = tc->version(err);
    CHECK(!v.empty());
    CHECK(err.empty());
    // esbuild versions look like "0.28.1" — assert a dotted, digit-led shape without pinning it.
    CHECK(v.find('.') != std::string::npos);
    CHECK(v[0] >= '0' && v[0] <= '9');
}

static void test_transpile_strips_types()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TranspileOptions opts;
    opts.format = cts::ModuleFormat::Esm; // plain transpile, no bundler wrapper
    cts::TranspileResult r = tc->transpile(example("util.ts"), opts);
    CHECK(r.ok);
    CHECK(r.diagnostics.empty());
    CHECK(contains(r.js, "function scale")); // the function survives
    CHECK(contains(r.js, "x * 3"));          // the body survives
    CHECK(contains(r.js, "export"));         // esm export emitted
    CHECK(!contains(r.js, ": number"));      // the TypeScript type annotations are stripped
}

static void test_bundle_inlines_imports_both_ways_shape()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Iife; // self-executing module (the V8-eval shape)
    cts::TranspileResult r = tc->transpile(example("game.ts"), opts);
    CHECK(r.ok);
    CHECK(r.diagnostics.empty());
    CHECK(contains(r.js, "(() =>"));                // wrapped in an IIFE
    CHECK(contains(r.js, "function scale"));        // util.ts was inlined by --bundle
    CHECK(contains(r.js, "hostBias(7)"));           // JS -> host call preserved
    CHECK(contains(r.js, "globalThis.update"));     // host -> JS export preserved
    CHECK(!contains(r.js, "import"));               // the import statement was resolved away
}

static void test_transpile_error_surfaces_catalog_code()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TranspileOptions opts; // no bundle -> a plain transpile failure
    cts::TranspileResult r = tc->transpile(example("bad_syntax.ts"), opts);
    CHECK(!r.ok);
    CHECK(r.js.empty());
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == std::string(cts::kTsTranspileFailedCode));
    CHECK(!r.diagnostics[0].message.empty()); // carries esbuild's human-facing text
    CHECK(contains(r.diagnostics[0].file, "bad_syntax.ts"));
}

static void test_bundle_error_surfaces_bundle_code()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TranspileOptions opts;
    opts.bundle = true; // an unresolved import only fails under --bundle
    cts::TranspileResult r = tc->transpile(example("bad_import.ts"), opts);
    CHECK(!r.ok);
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == std::string(cts::kTsBundleFailedCode));
    CHECK(!r.diagnostics[0].message.empty());
}

static void test_missing_input_is_a_diagnostic()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TranspileOptions opts;
    cts::TranspileResult r = tc->transpile(example("no_such_file.ts"), opts);
    CHECK(!r.ok);
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == std::string(cts::kTsTranspileFailedCode));
}

int main()
{
    test_available_and_version();
    test_transpile_strips_types();
    test_bundle_inlines_imports_both_ways_shape();
    test_transpile_error_surfaces_catalog_code();
    test_bundle_error_surfaces_bundle_code();
    test_missing_input_is_a_diagnostic();
    if (tstest::g_failures == 0)
    {
        std::printf("ts toolchain: all checks passed\n");
    }
    return tstest::g_failures == 0 ? 0 : 1;
}
