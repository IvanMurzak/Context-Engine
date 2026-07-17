#!/usr/bin/env python3
"""R-VER-004 versioned-fetch VERIFY SEAM (R-SEC-009 / DESIGN-DECISIONS L-58, task a08).

R-VER-004 installs every engine version side-by-side under ``<root>/versions/<semver>/`` and a
project's engine pin selects which one serves it (docs/versioned-install.md). The
resolver/fetcher/launcher machinery is a SECOND-release deliverable — but its rule 5 is a day-one
contract: *on-demand fetch of a pinned engine version is signed + verified against the trust root
(R-SEC-009), verify-before-EXECUTE, fail closed.* This module is that seam, landed now: the future
fetcher calls it on a downloaded version archive BEFORE it is unpacked/executed, and refuses to
install or run any version whose archive does not verify against the pinned production trust root.

It is a thin, purpose-named wrapper over ``tools/verify_artifact.py`` (the one shared gate), so the
mechanism, trust root, principal, and namespace are IDENTICAL to every other first-party fetch path
(the R-BUILD-004 export template / engine-fetched toolchain seams in the C++ build, and docs/signing.md).
No third-party Python dependency: verification is ``ssh-keygen -Y verify``, present on every runner.

CLI (mirrors verify_artifact.py's exit taxonomy):
    python3 tools/versioned_fetch.py --archive versions/0.1.0/context-0.1.0.tar.zst \
        --signature versions/0.1.0/context-0.1.0.tar.zst.sig [--trust-root <allowed_signers>]

Exit codes (any non-zero == REFUSED, do NOT unpack/execute the fetched version):
    0  verified — the fetched version is authentic under the pinned trust root; safe to execute.
    1  verification FAILED (bad / missing / tampered / untrusted-key / wrong-namespace signature).
    2  configuration/usage error (trust root or archive missing, or ssh-keygen absent). Still a refusal.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import verify_artifact
from verify_artifact import (
    CONFIG_ERROR,
    DEFAULT_IDENTITY,
    DEFAULT_NAMESPACE,
    DEFAULT_TRUST_ROOT,
    OK,
    REFUSED,
)


@dataclass(frozen=True)
class FetchVerdict:
    """The verify-before-execute verdict for a fetched engine version.

    code   -- OK / REFUSED / CONFIG_ERROR (mirrors verify_artifact's process exit taxonomy).
    detail -- human-readable one-liner (ssh-keygen's message or the config/refusal reason).
    ok     -- True only when the archive verified (safe to unpack + execute).
    """

    code: int
    detail: str

    @property
    def ok(self) -> bool:
        return self.code == OK


def verify_fetched_version(
    archive: Path,
    signature: Path,
    *,
    trust_root: Path = DEFAULT_TRUST_ROOT,
    identity: str = DEFAULT_IDENTITY,
    namespace: str = DEFAULT_NAMESPACE,
) -> FetchVerdict:
    """Verify a fetched pinned-version `archive` against its detached `signature` BEFORE it is
    unpacked/executed into ``versions/<semver>/`` (R-VER-004 rule 5 / R-SEC-009).

    FAILS CLOSED: any non-OK result from the pinned gate REFUSES the version — the pin is only
    meaningful if the fetched artifact is authenticated before it ever runs. Never raises; a
    fetcher can branch on `.ok` (or the process exit code via `main`).
    """
    result = verify_artifact.verify_artifact(
        archive, signature, trust_root=trust_root, identity=identity, namespace=namespace
    )
    return FetchVerdict(result.code, result.detail)


def require_verified_before_execute(
    archive: Path,
    signature: Path,
    *,
    trust_root: Path = DEFAULT_TRUST_ROOT,
    identity: str = DEFAULT_IDENTITY,
    namespace: str = DEFAULT_NAMESPACE,
) -> None:
    """Fail-closed guard the second-release fetcher calls at the execute boundary: verify the
    fetched version and raise PermissionError on any refusal, so an unverified version can never
    reach an unpack/exec call. Raising (not returning) makes 'forgot to check the verdict' impossible."""
    verdict = verify_fetched_version(
        archive, signature, trust_root=trust_root, identity=identity, namespace=namespace
    )
    if not verdict.ok:
        raise PermissionError(
            f"refusing to execute fetched version {archive}: {verdict.detail} "
            "(R-VER-004 / R-SEC-009 verify-before-execute, fail closed)"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", required=True, type=Path,
                        help="the fetched version archive to verify before execute")
    parser.add_argument("--signature", required=True, type=Path,
                        help="the detached SSH signature (.sig) fetched beside the archive")
    parser.add_argument("--trust-root", type=Path, default=DEFAULT_TRUST_ROOT,
                        help=f"pinned allowed_signers trust root (default: {DEFAULT_TRUST_ROOT})")
    parser.add_argument("--identity", default=DEFAULT_IDENTITY,
                        help=f"expected signer principal (default: {DEFAULT_IDENTITY})")
    parser.add_argument("--namespace", default=DEFAULT_NAMESPACE,
                        help=f"SSH signature namespace (default: {DEFAULT_NAMESPACE})")
    args = parser.parse_args(argv)

    verdict = verify_fetched_version(
        args.archive, args.signature, trust_root=args.trust_root,
        identity=args.identity, namespace=args.namespace,
    )
    if verdict.ok:
        print(f"[versioned-fetch] OK: {args.archive} verified — safe to unpack + execute "
              f"(signer '{args.identity}', namespace '{args.namespace}')")
    else:
        label = "REFUSED (fail closed)" if verdict.code == REFUSED else "CONFIG ERROR (fail closed)"
        print(f"[versioned-fetch] {label}: {args.archive} — {verdict.detail}", file=sys.stderr)
    return verdict.code


if __name__ == "__main__":
    sys.exit(main())
