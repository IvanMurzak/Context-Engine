// SRI verification: happy path (sha512 + sha256), tamper -> Mismatch, unsupported/malformed ->
// fail-closed, and the strongest-algorithm-present rule.

#include "context/editor/pkg/base64.h"
#include "context/editor/pkg/integrity.h"
#include "context/editor/pkg/sha512.h"
#include "context/editor/filesync/sha256.h"
#include "pkg_test.h"

#include <string>

using namespace context::editor::pkg;

namespace
{
std::string sri_for_sha512(std::string_view bytes)
{
    const Sha512Digest d = sha512(bytes);
    return "sha512-" + base64_encode(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
}
std::string sri_for_sha256(std::string_view bytes)
{
    const auto d = context::editor::filesync::sha256(bytes);
    return "sha256-" + base64_encode(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
}
} // namespace

int main()
{
    const std::string payload = "the package tarball bytes";

    // Happy path — sha512.
    {
        const SriVerification v = verify_integrity(sri_for_sha512(payload), payload);
        CHECK(v.result == SriResult::Ok);
        CHECK(v.algorithm == "sha512");
    }
    // Happy path — sha256.
    {
        const SriVerification v = verify_integrity(sri_for_sha256(payload), payload);
        CHECK(v.result == SriResult::Ok);
        CHECK(v.algorithm == "sha256");
    }

    // Tamper: the SRI is valid for DIFFERENT bytes -> Mismatch (fail-closed).
    {
        const SriVerification v = verify_integrity(sri_for_sha512(payload), payload + "!");
        CHECK(v.result == SriResult::Mismatch);
    }
    // Tamper: a single flipped base64 char in the digest -> Mismatch.
    {
        std::string sri = sri_for_sha512(payload);
        sri[sri.size() - 2] = (sri[sri.size() - 2] == 'A') ? 'B' : 'A';
        const SriVerification v = verify_integrity(sri, payload);
        CHECK(v.result == SriResult::Mismatch);
    }

    // Strongest-supported-algorithm-present rule: a list with a GOOD sha256 but a BAD sha512 fails
    // (sha512 is authoritative; a weaker hash never rescues a stronger declared one).
    {
        std::string bad512 = sri_for_sha512(payload + "x"); // wrong sha512
        std::string good256 = sri_for_sha256(payload);      // correct sha256
        const SriVerification v = verify_integrity(bad512 + " " + good256, payload);
        CHECK(v.result == SriResult::Mismatch);
        CHECK(v.algorithm == "sha512");
    }
    // A list with a good sha512 AND good sha256 -> Ok on sha512.
    {
        const SriVerification v =
            verify_integrity(sri_for_sha512(payload) + " " + sri_for_sha256(payload), payload);
        CHECK(v.result == SriResult::Ok);
        CHECK(v.algorithm == "sha512");
    }

    // Unsupported algorithm alone -> fail-closed (never silently trusted).
    {
        const SriVerification v = verify_integrity("sha1-2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
                                                   payload);
        CHECK(v.result == SriResult::UnsupportedAlgorithm);
    }
    // A supported algorithm present alongside an unsupported one is still verified.
    {
        const SriVerification v =
            verify_integrity("sha1-abc " + sri_for_sha256(payload), payload);
        CHECK(v.result == SriResult::Ok);
        CHECK(v.algorithm == "sha256");
    }

    // Malformed SRI -> Malformed (fail-closed).
    CHECK(verify_integrity("", payload).result == SriResult::Malformed);
    CHECK(verify_integrity("not-an-sri-shape-without-a-real-alg", payload).result ==
          SriResult::UnsupportedAlgorithm); // parses a token, alg "not" unsupported
    CHECK(verify_integrity("sha512", payload).result == SriResult::Malformed); // no '-<digest>'
    CHECK(verify_integrity("sha512-", payload).result == SriResult::Malformed);

    PKG_TEST_MAIN_END();
}
