#!/usr/bin/env python3
"""Fetch + verify + stage the pinned dockview-core distribution for the M9 editor-core.

Design D2 / ``04-editor-web-app-docking-panels.md`` §2 / ``08-security.md`` §3. This is the
acquisition half of the SHA-pinned third-party channel for the editor's docking engine — the
publisher's OWN npm tarball, the canonical publisher artifact. It implements the SAME
**verify-before-use, fail-closed** posture as ``tools/fetch_esbuild.py`` / ``tools/fetch_tsc.py``
(the wgpu-native / CEF / rusty_v8 precedent codified in ``docs/signing.md``): third-party build
inputs are authenticated by their own publisher via TLS + SHA-pin, and are explicitly OUT of scope
for the engine first-party trust root, so this deliberately does NOT route through
``tools/verify_artifact.py``.

Two things make it differ from the esbuild/tsgo fetchers, both deliberate:

  * **Platform-independent.** dockview-core is JavaScript source, not a per-platform native binary,
    so there is ONE tarball and no version-coherence matrix to assert. The structural invariant
    enforced instead is **per-member SHA pinning**: each extracted file is verified on its own, so a
    tarball that matches its own pin but whose INNER layout changed is still refused.
  * **It is a shipped runtime dependency of the web layer**, not a build tool that stays behind.
    esbuild BUNDLES it into the editor-core asset, so it is license-gated —
    ``tools/license-allowlist.json`` records ``dockview-core -> MIT`` and the deny-by-default
    ``license-gate`` job covers the ``src/editor/webui/core/package.json`` declaration.

⚠ The pin is **owner-consent-gated and VERSION-PINNED** (``08`` §3, ratified at s1 2026-07-19):
bumping past 7.0.2, or adding any further ``dockview-*`` package, re-triggers the standing consent
gate. Do not bump this manifest as a routine maintenance action.

What it does, per invocation:
  1. Loads the pin manifest (``tools/dockview-toolchain.json``) — the single source of truth.
  2. Downloads the package tarball over TLS, verifies its SHA-256 against the pin (fail-closed).
  3. Extracts each pinned member to ``<dest>/<name>`` and verifies EACH extracted file against its
     own SHA-256 pin (fail-closed) — the inner-layout guard described above.
  4. Writes an idempotency stamp so a re-run with a matching pin + present output is a no-op (the
     CMake configure step re-enters this on every reconfigure).

Offline / air-gapped / test: ``--source <dir>`` reads the tarball from a local directory (by its
manifest file name) instead of the network — the SHA verification still runs (fail-closed), so the
offline path is exactly as safe.

Exit codes (mirrors tools/fetch_esbuild.py / tools/fetch_tsc.py / tools/verify_artifact.py):
  * 0 — staged; every artifact verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, missing --source file, download error,
        missing member).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "dockview-toolchain.json"

_CHUNK = 1 << 20

# Retry policy for transient upstream failures (Context-Engine#129): a single npm-registry / CDN
# 504 or timeout must not hard-fail the fetch. SHA-256 verification still runs AFTER the download
# completes (fail-closed), so retrying never weakens the pin.
_MAX_ATTEMPTS = 4
_BASE_DELAY = 3.0


class FetchError(Exception):
    """Raised for a configuration/usage problem (maps to exit 2)."""


class VerifyError(Exception):
    """Raised when an artifact fails its SHA-256 pin (maps to exit 1)."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(_CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def check_manifest(manifest: dict) -> tuple[str, str]:
    """Validate the pin manifest's required shape; return ``(package, version)``.

    Unlike the esbuild manifest there is no per-platform matrix to cross-check, so the structural
    assertion here is that the single tarball pin AND every member pin are present and non-empty —
    a manifest missing a member hash would otherwise stage an UNVERIFIED file.
    """
    package = manifest.get("package")
    version = manifest.get("version")
    if not package:
        raise FetchError("manifest missing 'package'")
    if not version:
        raise FetchError("manifest missing 'version'")
    if not manifest.get("tarball_sha256"):
        raise FetchError("manifest missing 'tarball_sha256'")
    if not manifest.get("tarball_file"):
        raise FetchError("manifest missing 'tarball_file'")
    if not manifest.get("tarball_url_template"):
        # Validated HERE so a typo'd manifest is reported as the config error it is (exit 2). Left
        # to `fetch`, the unconditional dereference raises a bare KeyError that escapes main()'s
        # handlers as exit 1 — the code this tool reserves for a fail-closed SHA REFUSAL.
        raise FetchError("manifest missing 'tarball_url_template'")
    members = manifest.get("members")
    if not members:
        raise FetchError("manifest missing 'members'")
    for name, spec in members.items():
        # The member KEY becomes a real filesystem write at <dest>/<name>, exactly like the
        # in-tarball path guarded in _extract_member — so it gets the same containment check. The
        # manifest is first-party today; a guard that covers only one of two sibling path fields is
        # the kind of asymmetry that stops being true after a schema change.
        if ".." in Path(name).parts or Path(name).is_absolute() or "/" in name or "\\" in name:
            raise FetchError(f"refusing unsafe member name '{name}' — must be a plain file name")
        if not spec.get("member"):
            raise FetchError(f"member '{name}' missing its in-tarball 'member' path")
        if not spec.get("sha256"):
            raise FetchError(f"member '{name}' missing its 'sha256' pin — refusing to stage "
                             f"an unverified file")
    return package, version


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
                print(f"[fetch_dockview] download attempt {attempt}/{attempts} of {url} failed "
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


def _extract_member(tarball: Path, member: str, out_path: Path) -> None:
    """Extract a single `member` from the npm tar.gz to `out_path`."""
    if ".." in Path(member).parts or member.startswith("/"):
        raise FetchError(f"refusing unsafe member path '{member}'")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as tar:
        try:
            info = tar.getmember(member)
        except KeyError as exc:
            raise FetchError(
                f"member '{member}' not found in {tarball.name} — layout changed?") from exc
        if not info.isfile():
            raise FetchError(f"member '{member}' is not a regular file")
        extracted = tar.extractfile(info)
        if extracted is None:
            raise FetchError(f"member '{member}' could not be read from {tarball.name}")
        with extracted, out_path.open("wb") as out:
            shutil.copyfileobj(extracted, out)


def fetch(manifest_path: Path, dest: Path, source: Path | None = None) -> dict:
    """Download/verify/stage the pinned dockview-core dist assets into `dest`.

    Returns a dict describing the staged layout. Raises FetchError (config) / VerifyError
    (fail-closed SHA refusal). Idempotent: a matching stamp + present outputs short-circuit.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot read manifest {manifest_path}: {exc}") from exc

    package, version = check_manifest(manifest)
    members = manifest["members"]

    stamp_path = dest / ".dockview-fetch-stamp.json"
    want_stamp = {
        "package": package,
        "version": version,
        "tarball_sha256": manifest["tarball_sha256"],
        "members": {name: spec["sha256"] for name, spec in sorted(members.items())},
    }
    outputs = {name: dest / name for name in members}

    if stamp_path.is_file() and all(p.is_file() for p in outputs.values()):
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"staged": {n: str(p) for n, p in outputs.items()},
                        "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dockviewfetch-") as tmp:
        work = Path(tmp)
        url = manifest["tarball_url_template"].format(package=package, version=version)
        tarball = _obtain(manifest["tarball_file"], url, source, work)
        _verify(tarball, manifest["tarball_sha256"], f"{package} tarball ({version})")
        for name, spec in members.items():
            out_path = outputs[name]
            # Stage through a sibling temp path and only move it into place AFTER its pin verifies:
            # extracting straight to `out_path` would leave a REFUSED member's rejected bytes at the
            # very path the build reads from. The stamp fast-path above re-checks only that the
            # outputs EXIST, never their hashes, so a later run whose manifest matches the stamp
            # would then report those rejected bytes as `cached` — a fail-closed refusal that
            # silently becomes a pass. The sibling location (not the temp dir) keeps the move on one
            # filesystem, so it is atomic.
            staging = out_path.with_name(out_path.name + ".tmp")
            try:
                _extract_member(tarball, spec["member"], staging)
                # Per-member verification: the tarball pin alone would not catch an inner-layout swap.
                _verify(staging, spec["sha256"], f"{package} member '{name}'")
                staging.replace(out_path)
            finally:
                staging.unlink(missing_ok=True)

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"staged": {n: str(p) for n, p in outputs.items()},
            "version": version, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fetch + verify the pinned dockview-core distribution.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/dockview-toolchain.json)")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging dir; the dist assets land directly in <dest>/")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the package tarball from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_dockview] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_dockview] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    names = ", ".join(sorted(result["staged"]))
    print(f"[fetch_dockview] {state} dockview-core {result['version']}: {names}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
