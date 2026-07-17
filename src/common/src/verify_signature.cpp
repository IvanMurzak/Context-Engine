// Verify-before-use signature gate (see verify_signature.h) — the C++ mirror of
// tools/verify_artifact.py. Delegates the crypto to `ssh-keygen -Y verify` through the hardened
// context::common::subprocess runner; adds no third-party dependency.

#include "context/common/verify_signature.h"

#include "context/common/subprocess.h"

#include <string>
#include <system_error>

namespace context::common
{

namespace
{

// The exit code std::system() reports on POSIX when the command itself could not be found
// (`sh -c` → 127). We map it to ConfigError (like the Python gate's "ssh-keygen not found") so an
// absent verifier is surfaced as broken plumbing rather than a signature refusal — though BOTH fail
// closed. Every other non-zero exit (a real verify failure, or Windows' own not-found code) is a
// fail-closed Refused: the caller must not use the artifact either way.
constexpr int kCommandNotFound = 127;

[[nodiscard]] bool exists_file(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

} // namespace

VerifyOutcome verify_signature(const std::filesystem::path& artifact,
                               const std::filesystem::path& signature,
                               const std::filesystem::path& trust_root, std::string_view identity,
                               std::string_view namespace_)
{
    // Pre-checks mirror verify_artifact.py's classification (and make the config/refusal split
    // deterministic without invoking ssh-keygen). A missing pinned trust root or artifact is a
    // configuration failure — we refuse rather than fall back to any implicit trust (non-TOFU root).
    if (!exists_file(trust_root))
        return {VerifyStatus::ConfigError,
                "pinned trust root not found: " + trust_root.string() +
                    " (fail closed — nothing is trusted without the pinned root; see docs/signing.md)"};
    if (!exists_file(artifact))
        return {VerifyStatus::ConfigError, "artifact not found: " + artifact.string()};
    // A missing signature is the canonical fail-closed case: an unsigned artifact is REFUSED.
    if (!exists_file(signature))
        return {VerifyStatus::Refused,
                "signature not found: " + signature.string() + " (unsigned => refused)"};

    // `ssh-keygen -Y verify` reads the signed data from stdin and checks it against the detached
    // signature, the pinned allowed_signers root, the expected principal, and the namespace — the
    // exact argv tools/verify_artifact.py uses. The artifact is streamed as stdin via a shell
    // redirection (the same `<`/`>` redirection the a06 smoke path uses through this runner), so a
    // large protected artifact is not buffered into memory.
    std::string command;
    try
    {
        command = subprocess::quote_argument("ssh-keygen") + " -Y verify -f " +
                  subprocess::quote_argument(trust_root.string()) + " -I " +
                  subprocess::quote_argument(std::string(identity)) + " -n " +
                  subprocess::quote_argument(std::string(namespace_)) + " -s " +
                  subprocess::quote_argument(signature.string()) + " < " +
                  subprocess::quote_argument(artifact.string());
    }
    catch (const subprocess::MetacharacterError& ex)
    {
        // A path carrying a shell metacharacter cannot be safely launched — fail closed rather than
        // build an injectable command line.
        return {VerifyStatus::ConfigError,
                std::string("could not build a safe verify command: ") + ex.what()};
    }

    const int exit_code = subprocess::run_command(command);
    if (exit_code == 0)
        return {VerifyStatus::Ok, "signature OK"};
    if (exit_code == kCommandNotFound || exit_code == -1)
        return {VerifyStatus::ConfigError,
                "ssh-keygen could not be run (fail closed — cannot verify). Install OpenSSH."};
    return {VerifyStatus::Refused,
            "signature rejected by ssh-keygen -Y verify (fail closed; exit " +
                std::to_string(exit_code) + ")"};
}

} // namespace context::common
