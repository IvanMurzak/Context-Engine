// The OS code-signing hook (R-SEC-003 / R-BUILD-005, M8 tasks a10 + a13): the pure, deterministic half
// of `context build`'s signing report. Two shipped targets carry a runtime binary that MUST be
// code-signed to ship — Windows (a10, Authenticode) and macOS (a13, Developer ID + notarization). This
// module describes the signing requirement for a target (plan_signing) and, given the observed signing
// state of the produced binary, computes the machine-readable R-CLI-008 signing report the CLI folds
// into the build envelope (evaluate_signing).
//
// PRIMARY vs FALLBACK signing path (the plan echoes it, machine-readably):
//   * Windows (a10): the primary is Azure Trusted Signing (`azure/trusted-signing-action@v0` in the
//     protected `release` environment); the fallback is a developer-supplied Authenticode certificate
//     (signtool). BOTH produce an RFC-3161 timestamp — MANDATORY (short-lived certs would otherwise stop
//     verifying once the cert rotates).
//   * macOS (a13): the signature is produced by `codesign --options runtime --timestamp` with a
//     Developer ID Application identity (secure timestamp mandatory), and the artifact is then NOTARIZED
//     via `xcrun notarytool submit --wait` against an App-Store-Connect API key and the ticket STAPLED
//     (`xcrun stapler staple`) — macOS 15+ Gatekeeper hard-blocks an un-notarized build. notarization is
//     a plan attribute (notarization_required); the report's signed/unsigned state keys off the embedded
//     CODE SIGNATURE presence (below), and the CI job asserts the full notary loop (status==Accepted).
// NO secret ever lives here or in any committed file (R-SEC-003): the actual signing runs in CI with
// environment-scoped secrets; this pure module only PLANS + REPORTS.
//
// UNSIGNED IS NEVER SILENT (DoD): when signing is requested for a target that requires it but the
// produced binary carries no signature (e.g. a fork PR with no signing secrets), evaluate_signing
// returns an EXPLICIT `state: "unsigned"` + a machine-readable warning + the build.artifact_unsigned
// code — a legible WARNING state in the envelope, never a silent skip.
//
// The presence check is a PURE, cross-platform byte parse of the produced binary — no signtool/codesign,
// no OS API, no platform #if, so the local GCC gate exercises the exact same code the MSVC / Apple-clang
// CI legs do: pe_has_authenticode_signature (a signed PE/COFF carries a non-empty Certificate Table in
// the optional-header Security data directory) and macho_has_code_signature (a signed Mach-O carries an
// LC_CODE_SIGNATURE load command with a non-empty __LINKEDIT signature blob).

#pragma once

#include <string>
#include <string_view>

namespace context::editor::build
{

// The Authenticode (Windows, a10) signing method ids the plan reports (machine-readable, for an agent).
inline constexpr const char* kSigningMethodAuthenticode = "authenticode";
inline constexpr const char* kSigningPrimaryAzure = "azure-trusted-signing";
inline constexpr const char* kSigningFallbackDevCert = "developer-certificate";
inline constexpr const char* kSigningTool = "signtool";

// The Developer ID + notarization (macOS, a13) signing method ids. The `method` string matches the
// doctor's shared target→requirement enumeration (signing_requirements("macos")). `codesign` produces
// the detectable signature; `notarytool` submits it to Apple's notary service (the App-Store-Connect
// API-key primary path); there is no v1 fallback (only the API-key notary path ships).
inline constexpr const char* kSigningMethodDeveloperId = "developer-id-notarization";
inline constexpr const char* kSigningPrimaryAppleNotary = "apple-notary";
inline constexpr const char* kSigningToolCodesign = "codesign";
inline constexpr const char* kSigningToolNotary = "notarytool";

// The signing states evaluate_signing can report (folded into data.signing.state).
inline constexpr const char* kSigningStateNotRequired = "not-required"; // target needs no code-signing
inline constexpr const char* kSigningStateSigned = "signed";            // required + present + verified
inline constexpr const char* kSigningStateUnsigned = "unsigned";        // required but NO signature (WARN)

// The signing plan for a build target (pure + total). required=true only for targets with a v1
// code-signing prerequisite (windows Authenticode; macOS Developer ID + notarization); every other
// target yields required=false (the launcher/manifest are unsigned text either way — only a required
// target's runtime binary is signed).
struct SigningPlan
{
    bool required = false;
    std::string target;
    std::string method;           // kSigningMethodAuthenticode (win) / kSigningMethodDeveloperId (mac)
    std::string tool;             // kSigningTool (win) / kSigningToolCodesign (mac), "" otherwise
    std::string primary;          // kSigningPrimaryAzure (win) / kSigningPrimaryAppleNotary (mac)
    std::string fallback;         // kSigningFallbackDevCert (win); "" for macOS (no v1 fallback)
    bool timestamp_required = false;    // RFC-3161 (win) / codesign secure timestamp (mac) mandatory
    bool notarization_required = false; // macOS ONLY: Apple notarization + stapled ticket to ship (a13)
};

// Plan the signing requirement for a build target. Total + deterministic.
[[nodiscard]] SigningPlan plan_signing(std::string_view target);

// The observed signing inputs the CLI supplies (never a secret value): whether signing was REQUESTED
// (--sign), and whether the produced runtime binary actually CARRIES a signature (from a PE parse for
// windows / a Mach-O parse for macOS of the shipped binary bytes — see pe_has_authenticode_signature /
// macho_has_code_signature).
struct SigningInputs
{
    bool requested = false;      // --sign was passed
    bool artifact_signed = false; // the produced runtime binary carries an Authenticode signature
    bool binary_available = true; // the runtime binary was readable (false ⇒ nothing to inspect)
};

// The machine-readable signing report folded into the R-CLI-008 build envelope's data.signing. NEVER
// silent: a required-but-unsigned artifact carries `state: "unsigned"`, a `warning`, and the
// build.artifact_unsigned code. `requested=false` for a signing target reports state "unsigned" with a
// note that signing was not requested (so the operator sees the artifact is unsigned regardless).
struct SigningReport
{
    bool required = false;
    bool requested = false;
    bool signed_ = false;         // the produced binary carries a signature
    std::string state;            // kSigningState*
    std::string method;           // echoed from the plan
    std::string tool;
    std::string primary;
    std::string fallback;
    bool timestamp_required = false;
    bool notarization_required = false; // macOS ONLY: notarization is a further ship requirement (a13)
    std::string code;             // "" when signed/not-required; build.artifact_unsigned when unsigned
    std::string warning;          // "" unless unsigned — a machine-readable, never-silent warning
};

// Compute the signing report for (plan, inputs). Pure + total — never throws. A non-required target is
// "not-required"; a required target is "signed" only when the produced binary actually carries a
// signature, else "unsigned" with an explicit warning + the build.artifact_unsigned code.
[[nodiscard]] SigningReport evaluate_signing(const SigningPlan& plan, const SigningInputs& inputs);

// Does this PE/COFF image (the raw bytes of a shipped Windows `.exe`) carry an Authenticode signature?
// Pure, cross-platform byte parse: an Authenticode-signed PE has a NON-EMPTY Certificate Table in the
// optional-header Security data directory (IMAGE_DIRECTORY_ENTRY_SECURITY, index 4) — a WIN_CERTIFICATE
// blob appended to the file. Returns true iff that data directory's size is non-zero (a signature is
// present). false for a non-PE input, a truncated header, or an unsigned PE. Does NOT cryptographically
// verify the signature (that is signtool's job at sign time / the OS's at run time) — it detects
// PRESENCE, which is exactly what the never-silent unsigned-warning state keys off.
[[nodiscard]] bool pe_has_authenticode_signature(std::string_view pe_bytes);

// Does this Mach-O image (the raw bytes of a shipped macOS binary) carry an embedded code signature?
// Pure, cross-platform byte parse: a code-signed Mach-O has an LC_CODE_SIGNATURE (0x1D) load command
// whose linkedit_data_command references a NON-EMPTY __LINKEDIT signature blob (datasize != 0). Handles
// thin Mach-O (32/64-bit, either endianness) AND fat/universal binaries (FAT_MAGIC — signed iff any arch
// slice carries a signature; `codesign` signs every slice). Returns false for a non-Mach-O input, a
// truncated header, or an unsigned image. Like the PE parser it detects PRESENCE only — it does NOT
// cryptographically verify the signature (codesign at sign time / Gatekeeper at run time do that) and it
// does NOT prove NOTARIZATION (a stapled ticket / Apple's notary service — asserted by the CI job). No
// codesign, no macOS API, no platform #if — the local GCC gate exercises the exact code the Apple-clang
// CI leg does.
[[nodiscard]] bool macho_has_code_signature(std::string_view macho_bytes);

} // namespace context::editor::build
