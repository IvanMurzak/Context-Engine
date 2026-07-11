// Deterministic-build attestation tests (R-SIM-005 / L-54, R-QA-013 happy + failure coverage).
//
// The headline invariant: `deterministic:true` is PRODUCED from verified flags, never self-declared.
// The pure decision function verify_attestation() is exercised with SYNTHETIC probes so every
// fail-closed path is covered on the local GCC dev gate WITHOUT recompiling under a bad flag, and the
// REAL produce_attestation() is asserted honest under both the default build (false) and — when this
// TU is compiled under the deterministic preset — the deterministic build (true). Registered as the
// `determinism-attestation` ctest, so it runs under BOTH the default build's Determinism gate (where
// it must report an honest false) and the dedicated deterministic-preset CI job (where it must
// produce true).

#include "context/runtime/determinism/attestation.h"
#include "determinism_test.h"

#include <cstdio>

using namespace context::runtime::determinism;

int main()
{
    // --- PURE decision function: synthetic probes (fail-closed tamper paths) --------------------

    // A non-requested build is an HONEST false — deterministic stays false, no failure code (it is
    // not a deterministic build, which is not an error).
    {
        FpProbe p; // deterministic_requested defaults false
        const Attestation a = verify_attestation(p, "");
        CHECK(!a.requested);
        CHECK(!a.deterministic);
        CHECK(a.failure_code.empty());
    }

    // A forbidden fast-math flag leaked into a deterministic build → fail closed, NEVER a forged true.
    {
        FpProbe p;
        p.deterministic_requested = true;
        p.fast_math = true; // as the compiler would set __FAST_MATH__ from -ffast-math
        const Attestation a = verify_attestation(p, "-ffp-contract=off -ffast-math");
        CHECK(a.requested);
        CHECK(!a.deterministic);
        CHECK(a.failure_code == std::string(kAttestationFastMathForbidden));
    }

    // MSVC deterministic build without /fp:strict in effect → fail closed.
    {
        FpProbe p;
        p.deterministic_requested = true;
        p.is_msvc = true;
        p.msvc_strict_fp = false;
        const Attestation a = verify_attestation(p, "/fp:fast");
        CHECK(a.requested);
        CHECK(!a.deterministic);
        CHECK(a.failure_code == std::string(kAttestationStrictFpMissing));
    }

    // Requested but the build recorded no applied flags → cannot produce from verified flags, refuse.
    {
        FpProbe p;
        p.deterministic_requested = true;
        const Attestation a = verify_attestation(p, "");
        CHECK(a.requested);
        CHECK(!a.deterministic);
        CHECK(a.failure_code == std::string(kAttestationFlagsUnverified));
    }

    // Clean deterministic build (non-MSVC): no fast-math + recorded strict-FP flags → PRODUCE true.
    {
        FpProbe p;
        p.deterministic_requested = true;
        const Attestation a = verify_attestation(p, "-ffp-contract=off");
        CHECK(a.requested);
        CHECK(a.deterministic);
        CHECK(a.failure_code.empty());
        CHECK(a.applied_flags == "-ffp-contract=off");
    }

    // Clean deterministic build (MSVC strict-FP in effect) → PRODUCE true.
    {
        FpProbe p;
        p.deterministic_requested = true;
        p.is_msvc = true;
        p.msvc_strict_fp = true;
        const Attestation a = verify_attestation(p, "/fp:strict");
        CHECK(a.requested);
        CHECK(a.deterministic);
        CHECK(a.failure_code.empty());
    }

    // to_json is well-formed for both a produced-true and a failure record.
    {
        FpProbe ok;
        ok.deterministic_requested = true;
        const std::string j = to_json(verify_attestation(ok, "-ffp-contract=off"));
        CHECK(j.find("\"deterministic\":true") != std::string::npos);
        CHECK(j.find("\"appliedFlags\":\"-ffp-contract=off\"") != std::string::npos);

        FpProbe bad;
        bad.deterministic_requested = true;
        bad.fast_math = true;
        const std::string jf = to_json(verify_attestation(bad, "-ffast-math"));
        CHECK(jf.find("\"deterministic\":false") != std::string::npos);
        CHECK(jf.find("\"failureCode\":\"determinism.attestation_fastmath_forbidden\"") !=
              std::string::npos);
    }

    // --- the REAL build attestation --------------------------------------------------------------
    const Attestation real = produce_attestation();
    std::printf("[attestation] %s\n", to_json(real).c_str());

#if defined(CONTEXT_DETERMINISTIC)
    // Under the deterministic preset the build MUST produce deterministic:true from the verified,
    // actually-applied strict-FP flags (the dedicated deterministic CI job asserts exactly this on
    // clang / Apple clang / MSVC).
    CHECK(real.requested);
    CHECK(real.deterministic);
    CHECK(!real.applied_flags.empty());
    CHECK(real.failure_code.empty());
#else
    // Under the default (non-deterministic) build the attestation is an HONEST false — never a forged
    // true (R-SIM-005). This is the branch the default 3-OS Determinism gate exercises.
    CHECK(!real.requested);
    CHECK(!real.deterministic);
    CHECK(real.failure_code.empty());
#endif

    DETERMINISM_TEST_MAIN_END();
}
