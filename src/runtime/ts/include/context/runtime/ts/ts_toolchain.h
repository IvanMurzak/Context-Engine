// Backend-agnostic TypeScript-toolchain interface — the M3 TS scripting tier's build half
// (issue #83 / L-61 / R-LANG-002/004). This header is deliberately STL-only (no esbuild/V8
// types leak through it): it is the toolchain seam, mirroring runtime/js's JsEngine seam.
//
// Task 2b-i scope: transpile + bundle authored TypeScript to a single JavaScript module that
// the runtime/js V8 host (task 2a) evaluates. The toolchain shells out to the SHA-pinned
// esbuild prebuilt (tools/ts-toolchain.json + tools/fetch_esbuild.py) — a build-TIME native
// binary, NOT linked into the engine — so this driver builds + runs on EVERY toolchain
// (including the local Strawberry-GCC Windows dev gate), unlike the V8-linking runtime/js.
//
// Deferred seams (documented, NOT built here — see README.md § Deferred seams):
//   * A tsc-class SEMANTIC typecheck (--noEmit) whose findings surface as `ts.type_error`
//     envelopes through the R-CLI-008 catalog — the author->typecheck->fix loop (follow-up).
//   * Wiring the transpile as a derivation-graph node cached per R-FILE-010 (follow-up).
//   * R-LANG-010 declarative-component TS-accessor codegen (task 2b-ii) plugs in as an extra
//     generated input to the bundle; R-LANG-009 zero-copy views (task 3) and R-SEC-005
//     engine-driven npm install (task 5, --ignore-scripts + lockfile integrity) are separate.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace context::runtime::ts
{

// Output module format esbuild emits. `Iife` self-executes at eval time (top-level statements
// run when the runtime/js host evals it — the shape the V8 both-ways integration uses);
// `Esm` emits `export`s (a plain transpile with no bundler wrapper).
enum class ModuleFormat
{
    Iife,
    Esm,
};

// One transpile/bundle diagnostic, carrying the STABLE R-CLI-008 catalog code so a CLI/RPC
// caller branches on the failure class without parsing the message. Task 2b-i emits transpile-
// class codes only (kTsTranspileFailedCode / kTsBundleFailedCode); the semantic-typecheck
// `ts.type_error` code is the deferred follow-up (see header note above).
struct TsDiagnostic
{
    std::string code;    // an error_catalog.h code, e.g. "ts.transpile_failed"
    std::string message; // the tool's human-facing diagnostic text (esbuild stderr)
    std::string file;    // the source file the failure is attributed to (may be empty)
};

// The stable catalog codes this toolchain emits. DEFINED here (a runtime-tier constant) and
// REGISTERED in src/editor/contract/error_catalog.cpp — the same promote-a-local-string pattern
// bridge::kScopeDeniedCode uses, so runtime/ts does not link the editor/contract layer.
inline constexpr std::string_view kTsTranspileFailedCode = "ts.transpile_failed";
inline constexpr std::string_view kTsBundleFailedCode = "ts.bundle_failed";

struct TranspileOptions
{
    bool bundle = false;                       // resolve + inline imports (esbuild --bundle)
    ModuleFormat format = ModuleFormat::Esm;   // esbuild --format
};

// The result of a transpile/bundle. On success `ok == true` and `js` holds the emitted module;
// on failure `ok == false`, `js` is empty, and `diagnostics` is non-empty (each carrying a
// catalog code). Deterministic: same inputs -> same bytes (esbuild is run-deterministic).
struct TranspileResult
{
    bool ok = false;
    std::string js;
    std::vector<TsDiagnostic> diagnostics;
};

// The build-half of the TS tier. One backend today (esbuild); the interface is the seam so a
// future backend (or the deferred in-VM tsc) is a clean swap, exactly like JsEngine.
class TsToolchain
{
public:
    virtual ~TsToolchain() = default;

    virtual std::string_view name() const = 0;             // "esbuild"
    virtual std::string version(std::string& err) const = 0; // the tool's version string

    // Transpile (and optionally bundle) the TypeScript at `tsFilePath` into a JS module. The
    // path must name an existing .ts file readable by the tool; `bundle` resolves its imports.
    virtual TranspileResult transpile(const std::string& tsFilePath,
                                      const TranspileOptions& opts) = 0;
};

// true when an esbuild binary exists + is invocable at `esbuildBinaryPath`. The CMake configure
// stages it via tools/fetch_esbuild.py and forwards the path (CONTEXT_ESBUILD_PATH) to the tests.
[[nodiscard]] bool esbuildToolchainAvailable(const std::string& esbuildBinaryPath);

// Construct the esbuild-backed toolchain. Returns nullptr + fills `err` when the binary is
// missing/uninvokable at `esbuildBinaryPath`.
[[nodiscard]] std::unique_ptr<TsToolchain> createEsbuildToolchain(std::string esbuildBinaryPath,
                                                                  std::string& err);

} // namespace context::runtime::ts
