// Verify-before-use gate tests (R-SEC-009 / L-58, task a08) — the C++ mirror of the fail-closed
// coverage in tools/tests/test_verify_artifact.py. Exercises context::common::verify_signature against
// the SAME committed TEST-ONLY fixtures (tools/tests/fixtures/: a test signer's allowed_signers, a
// sample artifact, a pre-made detached signature — no private key is committed): a valid signature by
// the pinned test key verifies; a tampered / missing / untrusted-key / wrong-namespace / wrong-identity
// signature, a missing pinned trust root, and a missing artifact all FAIL CLOSED. The crypto paths need
// `ssh-keygen` (present on every CI runner); if absent they are skipped while the config paths still run.

#include "context/common/verify_signature.h"

#include "common_test.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using context::common::classify_verify_exit_code;
using context::common::verify_signature;
using context::common::VerifyStatus;
using Shell = context::common::subprocess::Shell;

namespace
{

// CONTEXT_VERIFY_FIXTURES_DIR is baked in by CMake — the committed tools/tests/fixtures directory.
const fs::path kFixtures = fs::path(CONTEXT_VERIFY_FIXTURES_DIR);
const fs::path kTrustRoot = kFixtures / "allowed_signers";      // pins principal "context-engine-test"
const fs::path kArtifact = kFixtures / "sample-artifact.bin";
const fs::path kSignature = kFixtures / "sample-artifact.bin.sig";

constexpr std::string_view kTestIdentity = "context-engine-test";
constexpr std::string_view kNamespace = "context-engine-artifact";

fs::path unique_tmp(const std::string& tag)
{
    static int counter = 0;
    fs::path p = fs::temp_directory_path() / ("ctx-verify-" + tag + "-" + std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

std::string read_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main()
{
    // --- Exit-code taxonomy (pure, no ssh-keygen — both shell shapes verified from ANY host) ---------
    // The regression this whole file guards: a genuine verify FAILURE (tampered / wrong-namespace /
    // wrong-identity) must map to Refused, never ConfigError. Its exit code is SHELL-SPECIFIC —
    // -1 on the Windows-builtin OpenSSH ssh-keygen (cmd.exe passes the raw child code through), 255
    // on POSIX / Git-for-Windows — so the classifier is parameterized on Shell and tested on both.
    {
        // A verified signature is Ok on either shell.
        CHECK(classify_verify_exit_code(0, Shell::Cmd) == VerifyStatus::Ok);
        CHECK(classify_verify_exit_code(0, Shell::Posix) == VerifyStatus::Ok);

        // Windows / cmd.exe: -1 (Windows-builtin OpenSSH) and 255 (Git-for-Windows) are REAL refusals,
        // NOT plumbing errors — this is the exact bug that reddened build (windows-latest). Only a
        // truly-absent command (cmd.exe exit 1, "'ssh-keygen' is not recognized") is ConfigError.
        CHECK(classify_verify_exit_code(-1, Shell::Cmd) == VerifyStatus::Refused);
        CHECK(classify_verify_exit_code(255, Shell::Cmd) == VerifyStatus::Refused);
        CHECK(classify_verify_exit_code(1, Shell::Cmd) == VerifyStatus::ConfigError);

        // POSIX / sh: run_command() reports -1 for a shell-launch failure and 127 for command-not-found
        // (both ConfigError); ssh-keygen's own 255 verify failure is Refused.
        CHECK(classify_verify_exit_code(-1, Shell::Posix) == VerifyStatus::ConfigError);
        CHECK(classify_verify_exit_code(127, Shell::Posix) == VerifyStatus::ConfigError);
        CHECK(classify_verify_exit_code(255, Shell::Posix) == VerifyStatus::Refused);
    }

    // --- Config-class fail-closed paths (no ssh-keygen needed — deterministic pre-checks) ------------
    {
        // A missing pinned trust root is a ConfigError refusal (we never fall back to implicit trust).
        const auto miss_root =
            verify_signature(kArtifact, kSignature, kFixtures / "nope" / "allowed_signers",
                             kTestIdentity, kNamespace);
        CHECK(!miss_root.ok());
        CHECK(miss_root.status == VerifyStatus::ConfigError);

        // A missing artifact is a ConfigError — there is nothing to authenticate.
        const auto miss_artifact = verify_signature(kFixtures / "ghost.bin", kSignature, kTrustRoot,
                                                    kTestIdentity, kNamespace);
        CHECK(!miss_artifact.ok());
        CHECK(miss_artifact.status == VerifyStatus::ConfigError);

        // A missing signature is the canonical fail-closed refusal: unsigned => REFUSED.
        const fs::path tmp = unique_tmp("nosig");
        const auto miss_sig = verify_signature(kArtifact, tmp / "does-not-exist.sig", kTrustRoot,
                                               kTestIdentity, kNamespace);
        CHECK(!miss_sig.ok());
        CHECK(miss_sig.status == VerifyStatus::Refused);
    }

    // --- Crypto paths (need ssh-keygen) --------------------------------------------------------------
    // Probe availability via the happy path: on a host without OpenSSH it returns ConfigError; there we
    // skip the crypto assertions (config coverage above already ran). CI runners always have ssh-keygen.
    const auto happy = verify_signature(kArtifact, kSignature, kTrustRoot, kTestIdentity, kNamespace);
    const bool ssh_keygen_available = happy.ok() || happy.status == VerifyStatus::Refused;
    if (!ssh_keygen_available)
    {
        std::fprintf(stderr, "[verify-signature] ssh-keygen unavailable — skipping crypto paths\n");
        COMMON_TEST_MAIN_END();
    }

    // Happy path — a valid signature by the pinned test key verifies.
    CHECK(happy.ok());
    CHECK(happy.status == VerifyStatus::Ok);

    // Tampered artifact — the committed signature no longer matches → REFUSED.
    {
        const fs::path tmp = unique_tmp("tamper");
        const fs::path tampered = tmp / "sample-artifact.bin";
        write_bytes(tampered, read_bytes(kArtifact) + "\nInjected trailer\n");
        const auto r = verify_signature(tampered, kSignature, kTrustRoot, kTestIdentity, kNamespace);
        CHECK(r.status == VerifyStatus::Refused);
    }

    // Wrong namespace / wrong identity — a signature minted for one context cannot be replayed, and an
    // unpinned principal is untrusted → both REFUSED.
    {
        const auto ns = verify_signature(kArtifact, kSignature, kTrustRoot, kTestIdentity,
                                         "some-other-namespace");
        CHECK(ns.status == VerifyStatus::Refused);
        const auto id = verify_signature(kArtifact, kSignature, kTrustRoot, "not-a-pinned-principal",
                                         kNamespace);
        CHECK(id.status == VerifyStatus::Refused);
    }

    // The PINNED PRODUCTION root (default trust_root of the shipped gate) refuses the test-key
    // signature — the production root pins ONLY the production key (a08), so the test key is untrusted.
    {
        const fs::path prod_root = kFixtures / ".." / ".." / "trust-root" / "allowed_signers";
        const auto r = verify_signature(kArtifact, kSignature, prod_root, kTestIdentity, kNamespace);
        CHECK(!r.ok()); // fail closed — the test key is not the pinned production key
    }

    COMMON_TEST_MAIN_END();
}
