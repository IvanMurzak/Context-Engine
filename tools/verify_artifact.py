#!/usr/bin/env python3
"""Verify-before-use artifact gate for Context Engine (R-SEC-009 / DESIGN-DECISIONS L-58).

R-SEC-009 requires a PINNED cryptographic root of trust and PER-ARTIFACT DETACHED
signatures, with MANDATORY verify-before-use for every executable or trust-bearing
artifact (engine binaries, the pinned toolchain, export templates, fetched engine
versions — R-VER-004). Verification FAILS CLOSED: an artifact that does not verify
against the pinned root is REFUSED, not used with a warning.

This tool is the machine half of that gate. It authenticates an artifact against a
DETACHED signature and a pinned trust-root file committed in-repo, and fails closed on
a missing, malformed, tampered, or untrusted-key signature.

Mechanism — OpenSSH Ed25519 signatures (`ssh-keygen -Y verify`):
  * Ed25519 ONLY — the single standard algorithm (business/legal 2026-07-02: public
    standard-crypto ⇒ no EAR notification). No algorithm agility, no downgrade surface.
  * ZERO third-party Python dependencies: `ssh-keygen` ships with OpenSSH, present on
    every CI runner (ubuntu/macos/windows) and dev machine. Nothing is added to the
    `python-tests` job's `pip install` line, and no native crypto wheel is vendored.
  * The trust root is an OpenSSH `allowed_signers` file (see ALLOWED SIGNERS in
    ssh-keygen(1)) committed in-repo — the pinned, non-TOFU root R-SEC-009 mandates.
  * The signature is a detached armored `-----BEGIN SSH SIGNATURE-----` blob produced by
    `ssh-keygen -Y sign -n <namespace>`; the same `<namespace>` must match on verify, so
    a signature minted for one context cannot be replayed into another.

v1 posture (R-SEC-009 "v1 = first-party release signing"): the PRODUCTION trust root
(tools/trust-root/allowed_signers) is intentionally EMPTY until the first release mints
the single publisher key — so the default gate refuses everything, which is the correct
fail-closed state while nothing first-party is published yet. The mint + custody
procedure lives in docs/signing.md.

CLI:
    python3 tools/verify_artifact.py --artifact <path> --signature <path> \
        [--trust-root <allowed_signers>] [--identity <principal>] [--namespace <ns>]

Exit codes (any non-zero == REFUSED, do not use the artifact):
    0  verified — the artifact is authentic under the pinned trust root.
    1  verification FAILED — bad / missing / tampered / untrusted-key / wrong-namespace
       signature (the fail-closed refusal).
    2  configuration/usage error — trust root missing/unreadable, artifact missing, or
       ssh-keygen not found. Still a refusal (the gate could not run), surfaced distinctly
       so the plumbing can be fixed.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The pinned PRODUCTION trust root committed in-repo (empty until first release — see
# docs/signing.md). Overridable with --trust-root (the test suite points at its TEST-ONLY
# allowed_signers).
DEFAULT_TRUST_ROOT = REPO_ROOT / "tools" / "trust-root" / "allowed_signers"

# The release signer principal the production trust root will pin at first release.
DEFAULT_IDENTITY = "context-engine-release"

# SSH signature namespace domain-separating Context Engine artifact signatures from any
# other use of the same key. Signing MUST use the same value (`ssh-keygen -Y sign -n`).
DEFAULT_NAMESPACE = "context-engine-artifact"

# Exit codes.
OK = 0
REFUSED = 1
CONFIG_ERROR = 2

# Upper bound (seconds) on the ssh-keygen verify subprocess. A wedged verifier — or a
# pathological artifact stream — must fail closed PROMPTLY rather than stall the gate (and
# anything driving it, e.g. a CI/release step) indefinitely. 60s is far above any real
# verify time, so it never trips on a legitimate artifact.
SSH_KEYGEN_TIMEOUT_SECONDS = 60


@dataclass(frozen=True)
class VerifyResult:
    """Outcome of a verification attempt.

    code   -- one of OK / REFUSED / CONFIG_ERROR (the process exit code).
    ok     -- True only when the artifact verified (code == OK).
    detail -- human-readable one-liner (ssh-keygen's message or the config error).
    """

    code: int
    detail: str

    @property
    def ok(self) -> bool:
        return self.code == OK


def find_ssh_keygen() -> str | None:
    """Locate the ssh-keygen binary on PATH, or None if it is unavailable."""
    return shutil.which("ssh-keygen")


def verify_artifact(
    artifact: Path,
    signature: Path,
    *,
    trust_root: Path = DEFAULT_TRUST_ROOT,
    identity: str = DEFAULT_IDENTITY,
    namespace: str = DEFAULT_NAMESPACE,
) -> VerifyResult:
    """Verify `artifact` against a detached `signature` and a pinned `trust_root`.

    FAILS CLOSED on every abnormal path: a missing trust root, a missing artifact, a
    missing/tampered/untrusted signature, a namespace/identity mismatch, or an absent
    ssh-keygen all return a non-OK result. Nothing is ever "verified with a warning".
    """
    keygen = find_ssh_keygen()
    if not keygen:
        return VerifyResult(
            CONFIG_ERROR,
            "ssh-keygen not found on PATH — cannot verify (fail closed). Install OpenSSH.",
        )

    # A missing/unreadable pinned trust root is a configuration failure: we refuse rather
    # than fall back to any implicit trust (R-SEC-009 is a PINNED, non-TOFU root).
    if not trust_root.is_file():
        return VerifyResult(
            CONFIG_ERROR,
            f"pinned trust root not found: {trust_root} (fail closed — nothing is trusted "
            "without the pinned root; see docs/signing.md)",
        )

    # A missing artifact is a usage error — there is nothing to authenticate.
    if not artifact.is_file():
        return VerifyResult(CONFIG_ERROR, f"artifact not found: {artifact}")

    # A missing signature is the canonical fail-closed case: an unsigned artifact is
    # REFUSED, never used. (Classified REFUSED, not CONFIG_ERROR — an absent signature is
    # a verification failure, not broken plumbing.)
    if not signature.is_file():
        return VerifyResult(REFUSED, f"signature not found: {signature} (unsigned ⇒ refused)")

    # Opening the artifact for streaming is the only artifact-READ step that can raise here;
    # keep its handler narrow so a subprocess-launch failure below is not misattributed to an
    # artifact-read error. The artifact is STREAMED as the child's stdin (not buffered into a
    # Python bytes), so a large protected artifact does not double peak memory; a read error
    # still fails closed.
    try:
        data = artifact.open("rb")
    except OSError as exc:
        return VerifyResult(CONFIG_ERROR, f"cannot read artifact {artifact}: {exc}")

    # ssh-keygen -Y verify reads the signed data from stdin and checks it against the detached
    # signature file, the pinned allowed_signers trust root, the expected signer principal, and
    # the namespace. Non-zero exit == verification failed == refuse. A wedged verifier is
    # bounded by SSH_KEYGEN_TIMEOUT_SECONDS and a spawn failure fails closed on its own path —
    # both distinct from (and no longer misreported as) the artifact-read failure above.
    try:
        with data:
            proc = subprocess.run(  # noqa: S603 - fixed argv, no shell
                [
                    keygen, "-Y", "verify",
                    "-f", str(trust_root),
                    "-I", identity,
                    "-n", namespace,
                    "-s", str(signature),
                ],
                stdin=data,
                capture_output=True,
                timeout=SSH_KEYGEN_TIMEOUT_SECONDS,
            )
    except subprocess.TimeoutExpired:
        return VerifyResult(
            CONFIG_ERROR,
            f"ssh-keygen did not complete within {SSH_KEYGEN_TIMEOUT_SECONDS}s — "
            "cannot verify (fail closed).",
        )
    except OSError as exc:
        return VerifyResult(CONFIG_ERROR, f"cannot invoke ssh-keygen: {exc}")
    if proc.returncode == 0:
        return VerifyResult(OK, _decode(proc.stdout).strip() or "signature OK")

    detail = _decode(proc.stderr).strip() or _decode(proc.stdout).strip() or "signature rejected"
    return VerifyResult(REFUSED, detail)


def _decode(raw: bytes) -> str:
    return raw.decode("utf-8", errors="replace")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--artifact", required=True, type=Path, help="path to the artifact to verify")
    parser.add_argument("--signature", required=True, type=Path,
                        help="path to the detached SSH signature (.sig)")
    parser.add_argument("--trust-root", type=Path, default=DEFAULT_TRUST_ROOT,
                        help=f"pinned allowed_signers trust root (default: {DEFAULT_TRUST_ROOT})")
    parser.add_argument("--identity", default=DEFAULT_IDENTITY,
                        help=f"expected signer principal (default: {DEFAULT_IDENTITY})")
    parser.add_argument("--namespace", default=DEFAULT_NAMESPACE,
                        help=f"SSH signature namespace (default: {DEFAULT_NAMESPACE})")
    args = parser.parse_args(argv)

    result = verify_artifact(
        args.artifact,
        args.signature,
        trust_root=args.trust_root,
        identity=args.identity,
        namespace=args.namespace,
    )

    if result.ok:
        print(f"[verify] OK: {args.artifact} verified against {args.trust_root} "
              f"(signer '{args.identity}', namespace '{args.namespace}')")
        print(f"[verify]   {result.detail}")
    else:
        stream = sys.stderr
        label = "REFUSED (fail closed)" if result.code == REFUSED else "CONFIG ERROR (fail closed)"
        print(f"[verify] {label}: {args.artifact}", file=stream)
        print(f"[verify]   {result.detail}", file=stream)
    return result.code


if __name__ == "__main__":
    sys.exit(main())
