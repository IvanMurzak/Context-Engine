#!/usr/bin/env python3
"""Fetch + verify + stage the pinned tsgo (native TypeScript) prebuilt for the M3 TS typecheck.

Issue #85 (the 2b-i follow-up) / R-LANG-002/004 / R-CLI-008. This is the acquisition half of
the SHA-pinned third-party build-tool channel for the tsc-class SEMANTIC typechecker — the
TypeScript team's OWN per-platform npm packages (``@typescript/native-preview-<platform>``, the
canonical publisher artifacts). It implements the SAME **verify-before-use, fail-closed** posture
as ``tools/fetch_esbuild.py`` / ``tools/fetch_v8.py`` for a *third-party* build tool — the
wgpu-native / CEF / rusty_v8 precedent codified in ``docs/signing.md`` (third-party build tools
are authenticated by their own publisher via TLS + SHA-pin; they are explicitly OUT of scope for
the engine first-party trust root, so this deliberately does NOT route through
``tools/verify_artifact.py``).

tsgo is a standalone native Go binary — a build-TIME semantic typechecker driven with ``--noEmit``,
invoked as a subprocess, NOT linked into the shipped engine (like esbuild, unlike rusty_v8). So it
runs on EVERY toolchain including the local Ninja+Strawberry-GCC Windows dev gate; there is no
MSVC/Clang-ABI restriction and no stub backend. esbuild deliberately does NOT typecheck (it strips
types); tsgo CLOSES the agent author->typecheck->fix loop.

The ONE structural difference from ``fetch_esbuild.py``: each tsgo per-platform package ships the
binary (``package/lib/tsgo[.exe]``) ALONGSIDE its default ``lib.*.d.ts`` type library under
``package/lib/``. tsgo resolves those libs RELATIVE TO ITS OWN executable directory and PANICS if
they are missing ("bundled: lib.d.ts does not exist; this executable may be misplaced"). So this
script stages the WHOLE ``member_dir`` (``package/lib``) to ``<dest>/lib/`` — the binary lands next
to its libs — instead of a single binary member.

What it does, per invocation:
  1. Loads the pin manifest (``tools/tsc-toolchain.json``) — the single source of truth.
  2. Asserts VERSION-COHERENCE (the correctness invariant): every per-platform package entry's file
     name carries the SAME ``tsc_version``. A skew is refused before anything is downloaded.
  3. Downloads the ``<platform>`` package tarball (a gzipped npm tar) over TLS, verifies its
     SHA-256 against the pin (fail-closed), and extracts every regular file under ``member_dir`` to
     ``<dest>/<relpath-under-member_dir>`` (making the binary executable on unix).
  4. Writes an idempotency stamp so a re-run with a matching pin + present output is a no-op (the
     pipeline re-enters this configure step on every CI-fail retry).

Offline / air-gapped / test: ``--source <dir>`` reads the tarball from a local directory (by its
manifest file name) instead of the network — the SHA verification still runs (fail-closed), so the
offline path is exactly as safe.

Exit codes (mirrors tools/fetch_esbuild.py / tools/verify_artifact.py):
  * 0 — staged; the artifact verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, unknown platform, version skew, missing
        --source file, download error, missing binary member).
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
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "tsc-toolchain.json"

_CHUNK = 1 << 20

# Retry policy for transient upstream failures (mirrors fetch_esbuild.py, Context-Engine#129): a
# single npm-registry / CDN 504 or timeout must not hard-fail the fetch. SHA-256 verification still
# runs AFTER the download completes (fail-closed), so retrying never weakens the pin.
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
    """Assert every per-platform package is version-locked to ``tsc_version``.

    A lib/version skew would stage a different tsgo than the pins claim; enforce it structurally
    from the pin names before anything is downloaded. The tarball file name follows npm's
    scoped-package convention (``native-preview-<platform>-<version>.tgz`` for the
    ``@typescript/native-preview-<platform>`` package).
    """
    version = manifest.get("tsc_version")
    if not version:
        raise FetchError("manifest missing 'tsc_version'")
    platforms = manifest.get("platforms", {})
    if not platforms:
        raise FetchError("manifest missing 'platforms'")
    for name, spec in platforms.items():
        expected_file = f"native-preview-{name}-{version}.tgz"
        if spec.get("file") != expected_file:
            raise FetchError(
                f"version-coherence FAILED: platform '{name}' file '{spec.get('file')}' "
                f"!= '{expected_file}' (tsc_version={version})")
        if not spec.get("package", "").endswith(name):
            raise FetchError(
                f"version-coherence FAILED: platform '{name}' package "
                f"'{spec.get('package')}' does not match the platform key")
    return version


def _download_with_retry(url: str, dest: Path, *, attempts: int = _MAX_ATTEMPTS,
                         base_delay: float = _BASE_DELAY, sleep=time.sleep) -> None:
    """Download `url` to `dest` over TLS, retrying transient network failures with exponential
    backoff. Raises FetchError only after `attempts` failures. The SHA-256 pin is checked by the
    caller afterwards, so this resilience never bypasses verification."""
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
                print(f"[fetch_tsc] download attempt {attempt}/{attempts} of {url} failed "
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


def _extract_member_dir(tarball: Path, member_dir: str, binary: str, dest: Path) -> Path:
    """Extract every regular file under ``member_dir`` from the npm tar.gz into ``dest``, preserving
    the layout RELATIVE TO ``member_dir`` (so the binary lands next to its lib.*.d.ts). Returns the
    staged binary path. tsgo needs its whole lib dir adjacent — extracting only the binary makes it
    panic ('bundled: lib.d.ts does not exist')."""
    prefix = member_dir.rstrip("/") + "/"
    if ".." in Path(member_dir).parts or member_dir.startswith("/"):
        raise FetchError(f"refusing unsafe member_dir '{member_dir}'")
    staged_binary: Path | None = None
    with tarfile.open(tarball, "r:gz") as tar:
        members = [m for m in tar.getmembers() if m.isfile() and m.name.startswith(prefix)]
        if not members:
            raise FetchError(
                f"member_dir '{member_dir}' has no files in {tarball.name} — layout changed?")
        for info in members:
            rel = info.name[len(prefix):]  # path under member_dir, e.g. "tsgo" / "lib.d.ts"
            if ".." in Path(rel).parts:
                raise FetchError(f"refusing unsafe member path '{info.name}'")
            out_path = dest / rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            extracted = tar.extractfile(info)
            if extracted is None:
                raise FetchError(f"member '{info.name}' could not be read from {tarball.name}")
            with extracted, out_path.open("wb") as out:
                shutil.copyfileobj(extracted, out)
            if rel == binary:
                staged_binary = out_path
    if staged_binary is None:
        raise FetchError(
            f"binary '{binary}' not found under '{member_dir}' in {tarball.name} — layout changed?")
    # Make the binary executable on POSIX (npm tars carry the mode, but be explicit + robust).
    if os.name != "nt":
        mode = staged_binary.stat().st_mode
        staged_binary.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return staged_binary


def fetch(manifest_path: Path, platform: str, dest: Path,
          source: Path | None = None) -> dict:
    """Download/verify/stage the pinned tsgo binary + its lib dir for `platform` into `dest`.

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

    bin_out = dest / spec["binary"]
    stamp_path = dest / ".tsc-fetch-stamp.json"
    want_stamp = {
        "tsc_version": version,
        "platform": platform,
        "sha256": spec["sha256"],
        "member_dir": spec["member_dir"],
        "binary": spec["binary"],
    }

    if stamp_path.is_file() and bin_out.is_file():
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"binary": str(bin_out), "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tscfetch-") as tmp:
        work = Path(tmp)
        url = manifest["package_url_template"].format(platform=platform, tsc_version=version)
        tarball = _obtain(spec["file"], url, source, work)
        _verify(tarball, spec["sha256"], f"tsgo package ({platform})")
        staged = _extract_member_dir(tarball, spec["member_dir"], spec["binary"], dest)

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"binary": str(staged), "version": version, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch + verify the pinned tsgo prebuilt.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/tsc-toolchain.json)")
    parser.add_argument("--platform", required=True,
                        help="tsgo platform key, e.g. linux-x64 / darwin-arm64 / win32-x64")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging root; the binary lands in <dest>/tsgo[.exe] next to lib.*.d.ts")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the package tarball from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.platform, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_tsc] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_tsc] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    print(f"[fetch_tsc] {state} tsgo {result['version']} for {args.platform}: "
          f"binary={result['binary']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
