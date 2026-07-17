// Verify-before-use signature gate for first-party artifacts (R-SEC-009 / DESIGN-DECISIONS L-58,
// task a08) — the C++ half of the trust chain, the exact mirror of tools/verify_artifact.py.
//
// R-SEC-009 mandates a PINNED cryptographic root of trust and PER-ARTIFACT DETACHED signatures with
// MANDATORY verify-before-use for every trust-bearing artifact the engine consumes at build time —
// the per-platform export template / shipped runtime host binary (R-BUILD-004) and the engine-fetched
// (mirrored) toolchain artifact. Verification FAILS CLOSED: an artifact that does not verify against
// the pinned root is REFUSED, never used with a warning.
//
// Mechanism — OpenSSH Ed25519 detached signatures (`ssh-keygen -Y verify`), IDENTICAL to the Python
// gate (docs/signing.md): Ed25519 only (no algorithm agility / downgrade surface); the trust root is
// an OpenSSH `allowed_signers` file committed in-repo (the pinned, non-TOFU root); the signature is a
// detached `-----BEGIN SSH SIGNATURE-----` blob minted with `ssh-keygen -Y sign -n <namespace>`, and
// the same <namespace> must match on verify so a signature minted for one context cannot be replayed.
// The verifier is invoked through the hardened context::common::subprocess runner (no bespoke crypto),
// so a C++ build that has no Python/repo checkout can still gate on the shipped ssh-keygen.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace context::common
{

// The release signer principal the production trust root pins (allowed_signers principal field).
inline constexpr std::string_view kReleaseSignerIdentity = "context-engine-release";
// The signature namespace domain-separating Context Engine artifact signatures (`ssh-keygen -Y -n`).
inline constexpr std::string_view kArtifactNamespace = "context-engine-artifact";

// Outcome of a verify-before-use attempt (mirrors tools/verify_artifact.py's exit taxonomy).
enum class VerifyStatus
{
    Ok,          // verified — authentic under the pinned trust root (exit 0 in the Python gate).
    Refused,     // fail-closed refusal — missing / tampered / untrusted-key / wrong-ns signature.
    ConfigError, // the gate could not run — trust root/artifact missing, or ssh-keygen absent. Still
                 // a refusal (the caller must NOT use the artifact); surfaced distinctly for plumbing.
};

struct VerifyOutcome
{
    VerifyStatus status = VerifyStatus::ConfigError;
    std::string detail; // human-readable one-liner (the fail-closed default is a refusal).

    [[nodiscard]] bool ok() const noexcept { return status == VerifyStatus::Ok; }
};

// Verify `artifact` against its detached `signature` under the pinned `trust_root` (an OpenSSH
// allowed_signers file), the expected signer `identity`, and `namespace_`. FAILS CLOSED on every
// abnormal path — a missing trust root / artifact / signature, a tampered or untrusted-key signature,
// a namespace or identity mismatch, or an absent ssh-keygen all return a non-Ok outcome. Never throws;
// pure IO/subprocess (no engine-domain state), so it lives in context_common alongside subprocess.
[[nodiscard]] VerifyOutcome verify_signature(const std::filesystem::path& artifact,
                                             const std::filesystem::path& signature,
                                             const std::filesystem::path& trust_root,
                                             std::string_view identity = kReleaseSignerIdentity,
                                             std::string_view namespace_ = kArtifactNamespace);

} // namespace context::common
