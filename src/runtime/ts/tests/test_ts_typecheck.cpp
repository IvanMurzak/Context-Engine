// R-QA-013 tests for the tsgo TypeScript semantic typechecker (issue #85). Zero-dependency harness
// (the repo carries no C++ test framework — each test is a plain executable that CHECK()s its
// invariants and returns non-zero on any failure), mirroring test_ts_toolchain.cpp.
//
// This is a LOCAL gate (like the esbuild transpile driver, unlike runtime/js): tsgo is a native
// binary invoked as a subprocess, so it builds + runs under the local Strawberry-GCC Windows dev gate.
// It covers the typechecker in isolation — availability, version, a type-VALID pass, a type-INVALID
// file surfacing ts.type_error diagnostics with positions, a missing input — PLUS the headline duality
// that motivates a SEPARATE tool: esbuild transpiles a type-invalid file fine (it strips types), while
// the semantic typecheck rejects it. That is the exact gap the author->typecheck->fix loop closes.

#include <cstdio>
#include <memory>
#include <string>

#include "context/runtime/ts/ts_toolchain.h"
#include "context/runtime/ts/ts_typecheck.h"

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

#ifndef CONTEXT_TSGO_PATH
#error "CONTEXT_TSGO_PATH must be defined by CMake (the staged tsgo binary path)"
#endif
#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_TS_EXAMPLES_DIR
#error "CONTEXT_TS_EXAMPLES_DIR must be defined by CMake (the authored .ts examples dir)"
#endif

namespace
{
const std::string kTsgo = CONTEXT_TSGO_PATH;
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
    CHECK(cts::tscToolchainAvailable(kTsgo));
    CHECK(!cts::tscToolchainAvailable(kTsgo + "-does-not-exist"));

    std::string err;
    std::unique_ptr<cts::TsTypechecker> tc = cts::createTsgoTypechecker(kTsgo, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    CHECK(tc->name() == "tsgo");
    err.clear();
    const std::string v = tc->version(err);
    CHECK(!v.empty());
    CHECK(err.empty());
    // tsgo versions look like "7.0.0-dev.20260707.2" — assert a dotted, digit-led shape (the
    // "Version " label is stripped by the driver) without pinning the exact value.
    CHECK(v.find('.') != std::string::npos);
    CHECK(v[0] >= '0' && v[0] <= '9');
}

static void test_type_valid_source_passes()
{
    std::string err;
    std::unique_ptr<cts::TsTypechecker> tc = cts::createTsgoTypechecker(kTsgo, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    // A type-valid module reports ZERO diagnostics. util.ts (an existing clean fixture) too.
    for (const char* name : {"typecheck_ok.ts", "util.ts"})
    {
        cts::TypecheckResult r = tc->check(example(name));
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }
}

static void test_type_error_surfaces_catalog_code_with_positions()
{
    std::string err;
    std::unique_ptr<cts::TsTypechecker> tc = cts::createTsgoTypechecker(kTsgo, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TypecheckResult r = tc->check(example("type_error.ts"));
    CHECK(!r.ok);
    // type_error.ts carries TWO distinct semantic errors (a bad assignment + a bad argument); tsgo
    // reports EVERY error in one pass so the agent fixes them together.
    CHECK(r.diagnostics.size() == 2);
    bool sawTs2322 = false;
    bool sawTs2345 = false;
    for (const cts::TsDiagnostic& d : r.diagnostics)
    {
        CHECK(d.code == std::string(cts::kTsTypeErrorCode)); // the R-CLI-008 code, NOT a transpile code
        CHECK(!d.message.empty());                           // carries tsgo's human-facing text
        CHECK(contains(d.file, "type_error.ts"));            // attributed to the source file
        CHECK(d.line > 0);                                   // 1-based position parsed from tsgo
        CHECK(d.column > 0);
        if (contains(d.message, "TS2322"))
        {
            sawTs2322 = true;
        }
        if (contains(d.message, "TS2345"))
        {
            sawTs2345 = true;
        }
    }
    CHECK(sawTs2322); // string -> number assignment
    CHECK(sawTs2345); // string argument to a number parameter
}

static void test_missing_input_is_a_diagnostic()
{
    std::string err;
    std::unique_ptr<cts::TsTypechecker> tc = cts::createTsgoTypechecker(kTsgo, err);
    CHECK(tc != nullptr);
    if (!tc)
    {
        return;
    }
    cts::TypecheckResult r = tc->check(example("no_such_file.ts"));
    CHECK(!r.ok);
    CHECK(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].code == std::string(cts::kTsTypeErrorCode));
    CHECK(!r.diagnostics[0].message.empty());
}

// The headline duality (why the typecheck is a SEPARATE tool): esbuild TRANSPILES type_error.ts fine
// — it strips the type annotations without checking them, so ok==true — while the semantic typecheck
// REJECTS the very same file with ts.type_error. esbuild's failure class is a SYNTAX error, not a type
// error; only tsgo closes the type half of the author->typecheck->fix loop.
static void test_esbuild_transpiles_what_typecheck_rejects()
{
    std::string err;
    std::unique_ptr<cts::TsToolchain> esbuild = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(esbuild != nullptr);
    std::unique_ptr<cts::TsTypechecker> tsc = cts::createTsgoTypechecker(kTsgo, err);
    CHECK(tsc != nullptr);
    if (!esbuild || !tsc)
    {
        return;
    }

    cts::TranspileOptions opts;
    opts.format = cts::ModuleFormat::Esm; // plain transpile, no bundler wrapper
    cts::TranspileResult transpiled = esbuild->transpile(example("type_error.ts"), opts);
    CHECK(transpiled.ok);              // esbuild does NOT typecheck — it transpiles the file happily
    CHECK(transpiled.diagnostics.empty());
    CHECK(contains(transpiled.js, "forty-two")); // the (mistyped) value survives into the JS

    cts::TypecheckResult checked = tsc->check(example("type_error.ts"));
    CHECK(!checked.ok);                // the semantic typecheck catches what esbuild let through
    CHECK(!checked.diagnostics.empty());
    CHECK(checked.diagnostics[0].code == std::string(cts::kTsTypeErrorCode));
}

int main()
{
    test_available_and_version();
    test_type_valid_source_passes();
    test_type_error_surfaces_catalog_code_with_positions();
    test_missing_input_is_a_diagnostic();
    test_esbuild_transpiles_what_typecheck_rejects();
    if (tstest::g_failures == 0)
    {
        std::printf("ts typecheck: all checks passed\n");
    }
    return tstest::g_failures == 0 ? 0 : 1;
}
