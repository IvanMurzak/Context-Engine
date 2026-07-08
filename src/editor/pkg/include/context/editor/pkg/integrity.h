// Subresource-Integrity (SRI) verification for the R-SEC-005 install path. An npm lockfile pins each
// artifact with an `integrity` SRI string — `<alg>-<base64(digest)>`, optionally a whitespace-
// separated list of several. Verification is verify-before-use + fail-closed (R-SEC-009): a mismatch
// or an unverifiable (unsupported / malformed) SRI refuses the artifact, never uses it with a
// warning. v1 verifies `sha512` (npm's default) and `sha256`; a stronger unknown algorithm present
// alone is treated as unverifiable (fail-closed), never silently trusted.

#pragma once

#include <string>
#include <string_view>

namespace context::editor::pkg
{

enum class SriResult
{
    Ok,                   // the bytes matched a listed digest of the strongest supported algorithm
    Mismatch,             // a supported algorithm was named but the bytes did not match
    UnsupportedAlgorithm, // no listed hash names an algorithm this engine can verify
    Malformed,            // the SRI string is empty or not `<alg>-<base64>` shaped
};

struct SriVerification
{
    SriResult result = SriResult::Malformed;
    std::string algorithm; // the algorithm actually used to decide (e.g. "sha512"); empty when none
};

// Verify `bytes` against the SRI string `sri`. When several hashes are listed, the STRONGEST
// supported algorithm present is authoritative (sha512 > sha256): the bytes match if they equal ANY
// listed digest of that algorithm (SRI semantics). If the only listed algorithms are unsupported,
// the result is UnsupportedAlgorithm (fail-closed) — a weaker supported hash is NOT used to satisfy
// a stronger declared one.
[[nodiscard]] SriVerification verify_integrity(std::string_view sri, std::string_view bytes);

} // namespace context::editor::pkg
