// Deterministic-build attestation implementation (see attestation.h).

#include "context/runtime/determinism/attestation.h"

// The strict-FP flags the build ACTUALLY applied for this compiler, recorded by CMake at configure
// time (src/CMakeLists.txt passes -DCONTEXT_DETERMINISTIC_FLAGS="<flags>" to this library when
// CONTEXT_DETERMINISTIC is ON). Empty on a non-deterministic build. This is what makes the
// attestation a record of the applied flags rather than a hand-set literal in source.
#ifndef CONTEXT_DETERMINISTIC_FLAGS
#define CONTEXT_DETERMINISTIC_FLAGS ""
#endif

namespace context::runtime::determinism
{

namespace
{
// Minimal JSON string escaper — the applied-flags field is a compiler flag string (may carry no
// control chars, but escape defensively so the one-line record is always well-formed).
void append_escaped(std::string& out, std::string_view s)
{
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
}
} // namespace

Attestation verify_attestation(const FpProbe& probe, std::string_view applied_flags)
{
    Attestation a;
    a.requested = probe.deterministic_requested;
    a.applied_flags = std::string(applied_flags);

    if (!probe.deterministic_requested)
    {
        // Honest: this is not a deterministic build. deterministic stays false, no failure code —
        // a non-deterministic build is not an error, it just does not attest.
        return a;
    }

    // A deterministic build was requested — VERIFY the actually-applied flags before producing
    // deterministic:true (R-SIM-005: never a self-declared bit). Every check below fails CLOSED.

    // 1. No forbidden relaxed-FP flag may have leaked into the sim path. The compiler sets
    //    __FAST_MATH__ / _M_FP_FAST from the ACTUAL -ffast-math / /fp:fast flag, so this catches a
    //    fast-math flag a "deterministic" build must not carry (no fast-math anywhere the sim reaches).
    if (probe.fast_math)
    {
        a.failure_code = kAttestationFastMathForbidden;
        return a;
    }

    // 2. On MSVC the strict-FP model must be in effect (/fp:strict → _M_FP_STRICT). GCC/Clang express
    //    strict-FP through -ffp-contract=off + the absence of fast-math (checked above + recorded in
    //    the applied-flags string), which they do not surface as a single macro.
    if (probe.is_msvc && !probe.msvc_strict_fp)
    {
        a.failure_code = kAttestationStrictFpMissing;
        return a;
    }

    // 3. The build must have RECORDED the strict-FP flags it applied. An empty record means the
    //    attestation cannot be produced from verified flags — refuse rather than assume (fail closed).
    if (a.applied_flags.empty())
    {
        a.failure_code = kAttestationFlagsUnverified;
        return a;
    }

    // All checks passed: the build enforced strict-FP and recorded it → PRODUCE deterministic:true.
    a.deterministic = true;
    return a;
}

Attestation produce_attestation()
{
    return verify_attestation(probe_fp_environment(), CONTEXT_DETERMINISTIC_FLAGS);
}

std::string to_json(const Attestation& a)
{
    std::string out = "{";
    out += "\"deterministic\":";
    out += a.deterministic ? "true" : "false";
    out += ",\"requested\":";
    out += a.requested ? "true" : "false";
    out += ",\"appliedFlags\":\"";
    append_escaped(out, a.applied_flags);
    out += "\"";
    if (!a.failure_code.empty())
    {
        out += ",\"failureCode\":\"";
        append_escaped(out, a.failure_code);
        out += "\"";
    }
    out += "}";
    return out;
}

} // namespace context::runtime::determinism
