// Deterministic-build attestation (R-SIM-005 / L-54, anchored in R-SEC-009).
//
// Determinism is a WHOLE-BUILD property (R-SIM-005): strict-FP flags set engine-wide on the sim path
// (no fast-math anywhere the sim can reach), FMA / floating-point contraction pinned (never
// compiler-discretionary), a single shipped deterministic transcendental library, and NO platform
// `libm` on the sim path. A module claims `deterministic:true` ONLY via an attestation the build
// itself PRODUCES from the actually-applied, verified compiler flags — never a hand-set manifest bit
// that a build which did not enforce the flags could forge (R-SIM-005: "Attestation is
// PRODUCED/VERIFIED, not a trusted manifest bit"). Trust in a *supplied* attestation is anchored in
// R-SEC-009 (a third-party attestation is trusted only if signed by the trust root or reproduced by
// the harness — a v2 concern; v1 keeps the engine-produced attestation this module emits).
//
// This header exposes three pieces so the producer is honest AND testable:
//   * FpProbe / probe_fp_environment() — the floating-point environment as OBSERVED by the compiling
//     translation unit, read from compiler-set predefined macros (__FAST_MATH__, MSVC _M_FP_*). The
//     compiler sets those from the ACTUAL flag, not from us, so a forbidden fast-math flag that
//     leaked into a "deterministic" build is DETECTABLE — the attestation cannot claim determinism a
//     build did not deliver.
//   * verify_attestation(probe, applied_flags) — a PURE decision function: given a probe + the flags
//     the build recorded it applied, decide deterministic / the fail-closed code. Pure so a test can
//     feed a synthetic tampered probe and assert the code WITHOUT recompiling under the bad flag.
//   * produce_attestation() — the real attestation for THIS build: verify over the live probe + the
//     CMake-recorded applied flags (the CONTEXT_DETERMINISTIC_FLAGS compile definition).

#pragma once

#include <string>
#include <string_view>

namespace context::runtime::determinism
{

// The determinism.attestation_* fail-closed codes (source-of-truth strings; the contract error
// catalog registers these — the same promote-a-local-string pattern as editor/pkg's kInstall*Code,
// bridge's scope.denied, runtime/ts's kTs*Code, so this module does not link the contract layer).
// When the build cannot be verified deterministic, produce_attestation() returns one of these and
// NEVER deterministic:true (R-SEC-009 fail-closed).
inline constexpr const char* kAttestationFastMathForbidden =
    "determinism.attestation_fastmath_forbidden";
inline constexpr const char* kAttestationStrictFpMissing = "determinism.attestation_strict_fp_missing";
inline constexpr const char* kAttestationFlagsUnverified = "determinism.attestation_flags_unverified";

// The floating-point environment of ONE translation unit, read from compiler-set predefined macros.
struct FpProbe
{
    bool deterministic_requested = false; // CONTEXT_DETERMINISTIC defined in the compiling TU
    bool fast_math = false;               // __FAST_MATH__ / MSVC _M_FP_FAST — a forbidden relaxed-FP flag applied
    bool is_msvc = false;                 // the MSVC toolchain (its strict-FP is a distinct macro)
    bool msvc_strict_fp = false;          // MSVC _M_FP_STRICT — /fp:strict actually in effect
};

// Probe THIS translation unit's FP environment. `constexpr` + header-inline ON PURPOSE: the tamper
// test compiles it under a deliberately-bad flag set and observes the real compiler macros, so the
// probe reflects the flags that were genuinely applied to whoever compiles it (never a literal).
[[nodiscard]] constexpr FpProbe probe_fp_environment() noexcept
{
    FpProbe p;
#if defined(CONTEXT_DETERMINISTIC)
    p.deterministic_requested = true;
#endif
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    p.fast_math = true;
#endif
#if defined(_MSC_VER)
    p.is_msvc = true;
#if defined(_M_FP_STRICT)
    p.msvc_strict_fp = true;
#endif
#endif
    return p;
}

// The produced attestation.
struct Attestation
{
    bool requested = false;     // was a deterministic build requested (CONTEXT_DETERMINISTIC)
    bool deterministic = false; // the PRODUCED result: the build verified deterministic
    std::string applied_flags;  // the strict-FP flags the build recorded it applied (empty ⇒ none)
    std::string failure_code;   // a determinism.attestation_* code when requested-but-unverifiable; else empty
};

// PURE decision: given a probe + the flags the build recorded it applied, produce the attestation.
// A non-requested build reports an HONEST `deterministic:false` with no failure code (it simply is
// not a deterministic build — not an error). A requested build fails closed with a
// determinism.attestation_* code unless every verified-flag check passes.
[[nodiscard]] Attestation verify_attestation(const FpProbe& probe, std::string_view applied_flags);

// The real attestation for THIS build: verify over the live probe + the CMake-recorded applied flags.
[[nodiscard]] Attestation produce_attestation();

// Render an attestation as a canonical one-line JSON record (the build-produced artifact / log line).
[[nodiscard]] std::string to_json(const Attestation& a);

} // namespace context::runtime::determinism
