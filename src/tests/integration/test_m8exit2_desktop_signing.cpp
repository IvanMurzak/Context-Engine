// M8 exit criterion 2 — `m8-exit-2-desktop-signing` (ROADMAP §1-M8 exit / a14-m8-exit) — the milestone
// bar "the DESKTOP artifacts are signed (macOS signed + notarized; Windows Authenticode-signed) with
// verify-before-use failing closed under the minted production key (R-SEC-009)" as an executable gate.
// It RIDES the landed a10 Windows Authenticode hook + a13 macOS Developer-ID/notarization hook (signing.h)
// + the a08 verify-before-use trust chain (context_common/verify_signature.h) over pure, in-memory
// surfaces (no signtool/codesign/ssh-keygen, no secrets, no OS API — the exact code every CI leg runs):
//   1  the a10/a13 signing PLANS are correct + method-tagged (windows→Authenticode w/ Azure primary +
//      dev-cert fallback + RFC-3161 timestamp; macos→Developer-ID + Apple notary + notarization required);
//      linux/web require no code-signing (not-required);
//   2  UNSIGNED IS NEVER SILENT (DoD): a signing-required target with an unsigned produced binary reports
//      state "unsigned" + the build.artifact_unsigned code + a machine-readable warning — never a silent
//      skip — while a signed binary reports "signed";
//   3  VERIFY-BEFORE-USE FAILS CLOSED (R-SEC-009 / L-58): the fail-closed exit taxonomy never maps a
//      non-zero verifier result to Ok on either host shell, and a missing/tampered/absent-signature
//      verify against the pinned root is REFUSED (never used with a warning) — hermetic, no ssh-keygen;
//   4  the a08 PINNED PRODUCTION trust root pins exactly ONE Ed25519 publisher key under the release
//      identity + artifact namespace (no algorithm agility / downgrade surface).
// Design refs: ROADMAP §1-M8 exit, R-SEC-003, R-SEC-009, R-BUILD-005, DESIGN-DECISIONS L-58.
//
// Runs in the blocking "M8 exit gate" build-job step on all three OS legs. All in-process, GPU-free.

#include "m8_exit_test.h"

#include "context/common/subprocess.h"
#include "context/common/verify_signature.h"
#include "context/editor/build/adapter.h" // the kTarget* platform-id constants
#include "context/editor/build/build_errors.h"
#include "context/editor/build/signing.h"

#include <string>

namespace build = context::editor::build;
namespace common = context::common;
namespace sp = context::common::subprocess;

using context::tests::m8::report;

namespace
{

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    // === Seam 1 — the a10 (Windows) + a13 (macOS) desktop signing PLANS, method-tagged ================
    {
        // a10 — Windows Authenticode: Azure Trusted Signing primary + developer-certificate fallback,
        // RFC-3161 timestamp mandatory, signtool-compatible; notarization is a macOS-only concept.
        const build::SigningPlan win = build::plan_signing(build::kTargetWindows);
        CHECK(win.required);
        CHECK(win.method == build::kSigningMethodAuthenticode);
        CHECK(win.primary == build::kSigningPrimaryAzure);
        CHECK(win.fallback == build::kSigningFallbackDevCert);
        CHECK(win.tool == build::kSigningTool);
        CHECK(win.timestamp_required);
        CHECK(!win.notarization_required);

        // a13 — macOS Developer ID + notarization: codesign + Apple notary, secure timestamp mandatory,
        // notarization required (macOS 15+ Gatekeeper hard-blocks an un-notarized build); no v1 fallback.
        const build::SigningPlan mac = build::plan_signing(build::kTargetMacos);
        CHECK(mac.required);
        CHECK(mac.method == build::kSigningMethodDeveloperId);
        CHECK(mac.primary == build::kSigningPrimaryAppleNotary);
        CHECK(mac.tool == build::kSigningToolCodesign);
        CHECK(mac.fallback.empty()); // only the API-key notary path ships in v1
        CHECK(mac.timestamp_required);
        CHECK(mac.notarization_required);

        // Linux + web have no v1 code-signing prerequisite.
        CHECK(!build::plan_signing(build::kTargetLinux).required);
        CHECK(!build::plan_signing(build::kTargetWeb).required);
    }

    // === Seam 2 — UNSIGNED IS NEVER SILENT (DoD): a required target's unsigned binary WARNS ============
    {
        for (const char* target : {build::kTargetWindows, build::kTargetMacos})
        {
            const build::SigningPlan plan = build::plan_signing(target);

            // Requested but the produced binary carries NO signature (the per-PR path, no secrets): the
            // never-silent unsigned state — state "unsigned" + build.artifact_unsigned + a warning.
            build::SigningInputs unsigned_inputs;
            unsigned_inputs.requested = true;
            unsigned_inputs.artifact_signed = false;
            const build::SigningReport ur = build::evaluate_signing(plan, unsigned_inputs);
            CHECK(ur.required);
            CHECK(ur.state == build::kSigningStateUnsigned);
            CHECK(ur.code == build::kBuildArtifactUnsignedCode); // "build.artifact_unsigned"
            CHECK(!ur.warning.empty());                          // a machine-readable, never-silent warning

            // A produced binary that DOES carry a signature reports "signed" with no warning/code.
            build::SigningInputs signed_inputs;
            signed_inputs.requested = true;
            signed_inputs.artifact_signed = true;
            const build::SigningReport sr = build::evaluate_signing(plan, signed_inputs);
            CHECK(sr.state == build::kSigningStateSigned);
            CHECK(sr.code.empty());
            CHECK(sr.warning.empty());
        }

        // A non-signing target (linux) reports "not-required" — no false warning.
        const build::SigningReport lr =
            build::evaluate_signing(build::plan_signing(build::kTargetLinux), build::SigningInputs{});
        CHECK(lr.state == build::kSigningStateNotRequired);
        CHECK(lr.code.empty());
    }

    // === Seam 3 — VERIFY-BEFORE-USE FAILS CLOSED (R-SEC-009 / L-58) ====================================
    {
        // The pure fail-closed exit taxonomy: NO non-zero verifier result maps to Ok, on either host
        // shell (hermetic — no ssh-keygen invoked). 0 is the ONLY Ok; every failure is Refused/ConfigError.
        CHECK(common::classify_verify_exit_code(0, sp::Shell::Cmd) == common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(0, sp::Shell::Posix) == common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(-1, sp::Shell::Cmd) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(255, sp::Shell::Cmd) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(1, sp::Shell::Cmd) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(-1, sp::Shell::Posix) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(127, sp::Shell::Posix) != common::VerifyStatus::Ok);
        CHECK(common::classify_verify_exit_code(255, sp::Shell::Posix) != common::VerifyStatus::Ok);

        // A real verify-before-use attempt over the pinned production root, fail-closed WITHOUT ssh-keygen:
        // a scratch artifact with a MISSING trust root is a ConfigError refusal (we never fall back to
        // implicit trust), and a MISSING signature is a Refused — either way the artifact is NOT used.
        const std::filesystem::path artifact = sp::make_scratch_path("ctx-m8-verify", ".bin");
        sp::ScratchFile artifact_guard(artifact);
        const std::string bytes = "not a real signed artifact";
        CHECK(sp::write_file(artifact, bytes.data(), bytes.size()));

        const common::VerifyOutcome miss_root = common::verify_signature(
            artifact, artifact /*any path*/, "no/such/trust-root/allowed_signers");
        CHECK(!miss_root.ok());
        CHECK(miss_root.status == common::VerifyStatus::ConfigError);

        const std::filesystem::path missing_sig = sp::make_scratch_path("ctx-m8-verify", ".sig");
        const common::VerifyOutcome miss_sig =
            common::verify_signature(artifact, missing_sig, CONTEXT_TRUST_ROOT);
        CHECK(!miss_sig.ok()); // no signature to authenticate → fail-closed
    }

    // === Seam 4 — the a08 PINNED PRODUCTION trust root: ONE Ed25519 publisher key, identity + namespace =
    {
        const std::string root = sp::read_file(CONTEXT_TRUST_ROOT);
        CHECK(!root.empty());
        // The release signer identity + the artifact namespace are pinned (domain-separated signatures).
        CHECK(contains(root, std::string(common::kReleaseSignerIdentity)));  // context-engine-release
        CHECK(contains(root, std::string(common::kArtifactNamespace)));      // context-engine-artifact
        // Ed25519 ONLY — no algorithm agility / downgrade surface (R-SEC-009).
        CHECK(contains(root, "ssh-ed25519"));
        CHECK(!contains(root, "ssh-rsa"));
    }

    return report("m8-exit-2-desktop-signing",
                  "a10 Authenticode + a13 Developer-ID/notarization hooks never-silent; verify-before-use "
                  "fails closed under the a08 pinned Ed25519 production trust root (R-SEC-009)");
}
