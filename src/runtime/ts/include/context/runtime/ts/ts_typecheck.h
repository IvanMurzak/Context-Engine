// Backend-agnostic TypeScript SEMANTIC-typecheck interface — the M3 TS tier's author->typecheck->fix
// loop (issue #85, the 2b-i follow-up; R-LANG-002/004 / R-CLI-008). Sibling of ts_toolchain.h: that
// header is the BUILD half (esbuild transpile+bundle, which STRIPS types without checking them); this
// header is the CHECK half (a tsc-class --noEmit pass that surfaces type errors as `ts.type_error`).
//
// esbuild deliberately does not typecheck, so this needs a SEPARATE tsc-class tool. The one backend
// today is tsgo (microsoft/typescript-go — the native TypeScript compiler), a SHA-pinned build-TIME
// native binary staged by tools/tsc-toolchain.json + tools/fetch_tsc.py and invoked as a SUBPROCESS
// (never linked). Like esbuild — and UNLIKE runtime/js's V8 link — it builds + runs on EVERY toolchain
// including the local Strawberry-GCC Windows dev gate, so the typecheck ctest is a LOCAL gate: the
// author->typecheck->fix loop converges on the developer's own machine, not only at CI.
//
// This is STL-only (no tsgo/esbuild/V8 types leak through the seam), mirroring TsToolchain. The seam
// makes a future backend (e.g. an in-VM tsc) a clean swap. It reuses ts_toolchain.h's TsDiagnostic
// shape + the kTsTypeErrorCode catalog string so a caller reports transpile and typecheck findings
// through one R-CLI-008 envelope.

#pragma once

#include "context/runtime/ts/ts_toolchain.h" // TsDiagnostic + kTsTypeErrorCode

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace context::runtime::ts
{

// The result of a semantic typecheck. `ok == true` iff the typechecker found ZERO diagnostics (the
// source is type-valid). On failure `ok == false` and `diagnostics` is non-empty — each carries
// kTsTypeErrorCode plus tsgo's `TSxxxx` message and the 1-based (line, column) of the error. A single
// tsgo run reports EVERY type error in the file (not just the first), so the agent fixes them in one
// pass. Deterministic: the same source + toolchain version -> the same diagnostics.
struct TypecheckResult
{
    bool ok = false;
    std::vector<TsDiagnostic> diagnostics;
};

// The check-half of the TS tier. One backend today (tsgo); the interface is the seam so a future
// backend is a clean swap, exactly like TsToolchain.
class TsTypechecker
{
public:
    virtual ~TsTypechecker() = default;

    virtual std::string_view name() const = 0;               // "tsgo"
    virtual std::string version(std::string& err) const = 0; // the tool's version string

    // Semantically typecheck the TypeScript at `tsFilePath` with the tool's default lib + strict
    // settings (a --noEmit pass — no JS is written; the transpile is ts_toolchain.h's job). The path
    // must name an existing .ts file readable by the tool. A type-valid file -> ok==true, no
    // diagnostics; a type-INVALID (but syntactically valid) file -> ok==false with one
    // kTsTypeErrorCode diagnostic per reported error. A tool-invocation failure that yields no
    // parseable diagnostic (e.g. a missing input) still returns ok==false with one diagnostic so
    // nothing is silently lost.
    virtual TypecheckResult check(const std::string& tsFilePath) = 0;
};

// true when a tsgo binary exists + is invocable at `tscBinaryPath`. The CMake configure stages it
// (with its adjacent lib.*.d.ts) via tools/fetch_tsc.py and forwards the path (CONTEXT_TSGO_PATH).
[[nodiscard]] bool tscToolchainAvailable(const std::string& tscBinaryPath);

// Construct the tsgo-backed typechecker. Returns nullptr + fills `err` when the binary is
// missing/uninvokable at `tscBinaryPath`.
[[nodiscard]] std::unique_ptr<TsTypechecker> createTsgoTypechecker(std::string tscBinaryPath,
                                                                   std::string& err);

} // namespace context::runtime::ts
