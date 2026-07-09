#!/usr/bin/env python3
"""Fetch + verify + stage the pinned esbuild prebuilt for the M3 TypeScript toolchain.

Issue #83 / L-61 / R-LANG-002/004. This is the acquisition half of the SHA-pinned
third-party build-tool channel (esbuild's OWN per-platform ``@esbuild/<platform>`` npm
packages — the canonical publisher artifacts). It implements the SAME
**verify-before-use, fail-closed** posture as ``tools/fetch_v8.py`` for a *third-party*
build tool — the wgpu-native / CEF / rusty_v8 precedent codified in ``docs/signing.md``
(third-party build tools are authenticated by their own publisher via TLS + SHA-pin;
they are explicitly OUT of scope for the engine first-party trust root, so this
deliberately does NOT route through ``tools/verify_artifact.py``).

esbuild is a standalone native Go binary — a build-TIME transpiler/bundler invoked as a
subprocess, NOT linked into the shipped engine (unlike rusty_v8). So, unlike the V8
prebuilt, it runs on EVERY toolchain including the local Ninja+Strawberry-GCC Windows dev
gate; there is no MSVC/Clang-ABI restriction and no stub backend.

What it does, per invocation:
  1. Loads the pin manifest (``tools/ts-toolchain.json``) — the single source of truth.
  2. Asserts VERSION-COHERENCE (the correctness invariant): every per-platform package
     entry's file name carries the SAME ``esbuild_version``. A skew is refused before
     anything is downloaded.
  3. Downloads the ``<platform>`` package tarball (a gzipped npm tar) over TLS, verifies
     its SHA-256 against the pin (fail-closed), and extracts the SINGLE binary member
     (``package/bin/esbuild`` on unix, ``package/esbuild.exe`` on win32) to
     ``<dest>/bin/<extracted>`` (making it executable on unix).
  4. Writes an idempotency stamp so a re-run with a matching pin + present output is a
     no-op (the pipeline re-enters this configure step on every CI-fail retry).

Offline / air-gapped / test: ``--source <dir>`` reads the tarball from a local directory
(by its manifest file name) instead of the network — the SHA verification still runs
(fail-closed), so the offline path is exactly as safe.

Exit codes (mirrors tools/fetch_v8.py / tools/verify_artifact.py):
  * 0 — staged; the artifact verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, unknown platform, version skew,
        missing --source file, download error, missing binary member).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "ts-toolchain.json"

_CHUNK = 1 << 20

# Retry policy for transient upstream failures (Context-Engine#129): a single npm-registry / CDN
# 504 or timeout must not hard-fail the fetch. SHA-256 verification still runs AFTER the download
# completes (fail-closed), so retrying never weakens the pin.
_MAX_ATTEMPTS = 4
_BASE_DELAY = 3.0


class FetchError(Exception):
    """Raised for a configuration/usage problem (maps to exit 2)."""


class VerifyError(Exception):
    """Raised when a downloaded artifact fails its SHA-256 pin (maps to exit 1)."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(_CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def check_coherence(manifest: dict) -> str:
    """Assert every per-platform package is version-locked to ``esbuild_version``.

    A lib/version skew would stage a different esbuild than the pins claim; enforce it
    structurally from the pin names before anything is downloaded.
    """
    version = manifest.get("esbuild_version")
    if not version:
        raise FetchError("manifest missing 'esbuild_version'")
    platforms = manifest.get("platforms", {})
    if not platforms:
        raise FetchError("manifest missing 'platforms'")
    for name, spec in platforms.items():
        expected_file = f"{name}-{version}.tgz"
        if spec.get("file") != expected_file:
            raise FetchError(
                f"version-coherence FAILED: platform '{name}' file '{spec.get('file')}' "
                f"!= '{expected_file}' (esbuild_version={version})")
        if not spec.get("package", "").endswith(name):
            raise FetchError(
                f"version-coherence FAILED: platform '{name}' package "
                f"'{spec.get('package')}' does not match the platform key")
    return version


def _download_with_retry(url: str, dest: Path, *, attempts: int = _MAX_ATTEMPTS,
                         base_delay: float = _BASE_DELAY, sleep=time.sleep) -> None:
    """Download `url` to `dest` over TLS, retrying transient network failures with exponential
    backoff (Context-Engine#129). Raises FetchError only after `attempts` failures. The SHA-256
    pin is checked by the caller afterwards, so this resilience never bypasses verification."""
    last_exc: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=60) as resp, dest.open("wb") as out:  # noqa: S310
                shutil.copyfileobj(resp, out)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            last_exc = exc
            if attempt < attempts:
                delay = base_delay * (3 ** (attempt - 1))
                print(f"[fetch_esbuild] download attempt {attempt}/{attempts} of {url} failed "
                      f"({exc}); retrying in {delay:.0f}s", file=sys.stderr)
                sleep(delay)
    raise FetchError(f"download of {url} failed after {attempts} attempts: {last_exc}") from last_exc


def _obtain(name: str, url: str, source: Path | None, work: Path) -> Path:
    """Return a local path to the artifact `name`, from --source or by TLS download."""
    if source is not None:
        local = source / name
        if not local.is_file():
            raise FetchError(f"--source given but '{name}' not found in {source}")
        return local
    dest = work / name
    _download_with_retry(url, dest)
    return dest


def _verify(path: Path, expected_sha: str, what: str) -> None:
    actual = _sha256(path)
    if actual != expected_sha:
        raise VerifyError(
            f"SHA-256 mismatch for {what}: expected {expected_sha}, got {actual} — REFUSED")


def _extract_binary(tarball: Path, member: str, out_path: Path) -> None:
    """Extract the single binary `member` from the npm tar.gz to `out_path` (chmod +x on unix)."""
    if ".." in Path(member).parts or member.startswith("/"):
        raise FetchError(f"refusing unsafe member path '{member}'")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as tar:
        try:
            info = tar.getmember(member)
        except KeyError as exc:
            raise FetchError(
                f"binary member '{member}' not found in {tarball.name} — layout changed?") from exc
        if not info.isfile():
            raise FetchError(f"member '{member}' is not a regular file")
        extracted = tar.extractfile(info)
        if extracted is None:
            raise FetchError(f"member '{member}' could not be read from {tarball.name}")
        with extracted, out_path.open("wb") as out:
            shutil.copyfileobj(extracted, out)
    # Make the binary executable on POSIX (npm tars carry the mode, but be explicit + robust).
    if os.name != "nt":
        mode = out_path.stat().st_mode
        out_path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def fetch(manifest_path: Path, platform: str, dest: Path,
          source: Path | None = None) -> dict:
    """Download/verify/stage the pinned esbuild binary for `platform` into `dest`.

    Returns a dict describing the staged layout. Raises FetchError (config) / VerifyError
    (fail-closed SHA refusal). Idempotent: a matching stamp + present output short-circuits.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot read manifest {manifest_path}: {exc}") from exc

    version = check_coherence(manifest)
    platforms = manifest["platforms"]
    if platform not in platforms:
        raise FetchError(
            f"unknown platform '{platform}'; manifest has: {', '.join(sorted(platforms))}")
    spec = platforms[platform]

    bin_out = dest / "bin" / spec["extracted"]
    stamp_path = dest / ".esbuild-fetch-stamp.json"
    want_stamp = {
        "esbuild_version": version,
        "platform": platform,
        "sha256": spec["sha256"],
        "member": spec["member"],
    }

    if stamp_path.is_file() and bin_out.is_file():
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"binary": str(bin_out), "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="esbuildfetch-") as tmp:
        work = Path(tmp)
        url = manifest["package_url_template"].format(
            platform=platform, esbuild_version=version)
        tarball = _obtain(spec["file"], url, source, work)
        _verify(tarball, spec["sha256"], f"esbuild package ({platform})")
        _extract_binary(tarball, spec["member"], bin_out)

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"binary": str(bin_out), "version": version, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch + verify the pinned esbuild prebuilt.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/ts-toolchain.json)")
    parser.add_argument("--platform", required=True,
                        help="esbuild platform key, e.g. linux-x64 / darwin-arm64 / win32-x64")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging root; the binary lands in <dest>/bin/esbuild[.exe]")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the package tarball from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.platform, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_esbuild] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_esbuild] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    print(f"[fetch_esbuild] {state} esbuild {result['version']} for {args.platform}: "
          f"binary={result['binary']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
