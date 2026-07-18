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

[[nodiscard]] bool exists_file(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

} // namespace

VerifyStatus classify_verify_exit_code(int exit_code, subprocess::Shell shell) noexcept
{
    if (exit_code == 0)
        return VerifyStatus::Ok;

    // Distinguish "the verifier could not be launched at all" (ConfigError — broken plumbing, e.g. an
    // absent ssh-keygen) from "ssh-keygen ran and refused the signature" (Refused). BOTH fail closed
    // (VerifyOutcome::ok() is false either way); the split is diagnostic only. Which exit codes mean
    // which is host-shell-specific because subprocess::run_command()'s contract differs by shell —
    // see verify_signature.h and subprocess.h.
    if (shell == subprocess::Shell::Cmd)
    {
        // Windows / cmd.exe: run_command() returns the child's RAW exit code. A truly-absent command
        // makes cmd.exe exit 1 ("'ssh-keygen' is not recognized" / "cannot find the path"), so 1 is
        // the only not-runnable code. A genuine verify FAILURE exits -1 (Windows-builtin OpenSSH
        // ssh-keygen) or 255 (Git-for-Windows'), and every other non-zero is likewise a real refusal.
        return exit_code == 1 ? VerifyStatus::ConfigError : VerifyStatus::Refused;
    }
    // POSIX / sh: run_command() maps a shell-launch failure to -1 and `sh -c`'s command-not-found to
    // 127 — both mean the gate could not run. Any other non-zero (ssh-keygen's own 255, …) is Refused.
    return (exit_code == -1 || exit_code == 127) ? VerifyStatus::ConfigError : VerifyStatus::Refused;
}

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
    //
    // The trailing `1>&2` routes ssh-keygen's OWN chatter to STDERR: on success it prints a
    // `Good "<ns>" signature for <identity> ...` line to STDOUT, which — since run_command inherits
    // the caller's stdout — would otherwise CORRUPT a machine-readable envelope a caller emits on
    // stdout (e.g. `context build --toolchain-sig` prints the R-CLI-008 JSON envelope). The verdict
    // travels via the EXIT CODE (classify_verify_exit_code), never ssh-keygen's stdout text, so
    // diverting it to stderr is loss-free and keeps the fetch-verify refusal cleanly machine-readable
    // (R-SEC-009 fail-closed, R-CLI-008). Portable across cmd.exe and POSIX sh.
    std::string command;
    try
    {
        command = subprocess::quote_argument("ssh-keygen") + " -Y verify -f " +
                  subprocess::quote_argument(trust_root.string()) + " -I " +
                  subprocess::quote_argument(std::string(identity)) + " -n " +
                  subprocess::quote_argument(std::string(namespace_)) + " -s " +
                  subprocess::quote_argument(signature.string()) + " < " +
                  subprocess::quote_argument(artifact.string()) + " 1>&2";
    }
    catch (const subprocess::MetacharacterError& ex)
    {
        // A path carrying a shell metacharacter cannot be safely launched — fail closed rather than
        // build an injectable command line.
        return {VerifyStatus::ConfigError,
                std::string("could not build a safe verify command: ") + ex.what()};
    }

    const int exit_code = subprocess::run_command(command);
    switch (classify_verify_exit_code(exit_code, subprocess::host_shell()))
    {
    case VerifyStatus::Ok:
        return {VerifyStatus::Ok, "signature OK"};
    case VerifyStatus::ConfigError:
        return {VerifyStatus::ConfigError,
                "ssh-keygen could not be run (fail closed — cannot verify). Install OpenSSH."};
    case VerifyStatus::Refused:
        break;
    }
    return {VerifyStatus::Refused,
            "signature rejected by ssh-keygen -Y verify (fail closed; exit " +
                std::to_string(exit_code) + ")"};
}

} // namespace context::common
