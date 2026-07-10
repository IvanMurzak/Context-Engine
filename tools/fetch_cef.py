#!/usr/bin/env python3
"""Fetch + verify + stage the pinned CEF prebuilt for the M5 editor-GUI build substrate.

Issue #150 / L-15 / L-41 / R-UI-007 / R-SEC-009. This is the acquisition half of the SHA-pinned
third-party prebuilt channel for CEF (the Chromium Embedded Framework) — the official ``minimal``
binary distribution from the Spotify CDN (cef-builds.spotifycdn.com, the CEF project's own binary
host). It implements R-SEC-009's **verify-before-use, fail-closed** posture for a *third-party*
build lib — the wgpu-native / rusty_v8 precedent codified in ``docs/signing.md``: third-party libs
are authenticated by their own publisher via TLS + SHA-256 pin, and are explicitly OUT of scope for
the engine first-party trust root, so this deliberately does NOT route through
``tools/verify_artifact.py`` (the #76 Option-A signed-prebuilt carve-out).

Modeled on ``tools/fetch_v8.py`` — same download-with-retry, same fail-closed SHA-256 verify, same
``--source`` offline mode, same idempotency stamp. Differences: CEF ships ONE self-contained archive
per triple (a ``.tar.bz2`` carrying the whole distribution under a top-level prefix dir), so there is
no separate lib/header split and no lib-vs-header version-coherence check — just a filename↔version
coherence guard (the archive name must carry the pinned ``cef_version``).

What it does, per invocation:
  1. Loads the pin manifest (``tools/cef-prebuilt.json``) — the single source of truth.
  2. Resolves the ``<triple>`` archive entry and asserts the pinned filename carries ``cef_version``.
  3. Downloads the ``.tar.bz2`` over TLS (retry/backoff), verifies its SHA-256 against the pin
     (fail-closed), and extracts the CEF distribution into ``<dest>/`` with the single top-level
     ``cef_binary_…`` prefix dir stripped (so ``<dest>/include``, ``<dest>/Release``, ``<dest>/cmake``,
     ``<dest>/libcef_dll``, ``<dest>/CMakeLists.txt`` land directly). Symlinks + file modes are
     PRESERVED (the macOS ``Chromium Embedded Framework.framework`` relies on both).
  4. Writes an idempotency stamp so a re-run with matching pins + present outputs is a no-op (the
     build re-enters this configure step on every CI-fail retry).

Offline / air-gapped / test: ``--source <dir>`` reads the archive from a local directory (by its
manifest file name) instead of the network — the SHA-256 verification still runs (fail-closed), so
the offline path is exactly as safe.

Exit codes (mirrors tools/fetch_v8.py / tools/verify_artifact.py):
  * 0 — staged; the archive verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, unknown triple, version skew, missing --source
        file, download error, malformed archive).
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
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "cef-prebuilt.json"

_CHUNK = 1 << 20

# Retry policy for transient upstream failures (Context-Engine#129): a single CDN 504 or timeout
# must not hard-fail the fetch. SHA-256 verification still runs AFTER the download completes
# (fail-closed), so retrying never weakens the pin. Matches tools/fetch_v8.py.
_MAX_ATTEMPTS = 4
_BASE_DELAY = 3.0

# A sentinel that exists in every CEF distribution's stripped root — used for the cached short-circuit.
_ROOT_SENTINEL = Path("include") / "cef_version.h"


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


def _url_for(manifest: dict, archive: dict) -> str:
    """Build the CDN URL. The CEF filename carries '+' chars that MUST be %2B-escaped in the URL
    (the on-disk file name keeps the literal '+'); everything else in the name is URL-safe."""
    template = manifest.get("url_template")
    if not template:
        raise FetchError("manifest missing 'url_template'")
    return template.format(url_file=archive["file"].replace("+", "%2B"))


def resolve(manifest: dict, triple: str) -> tuple[str, dict]:
    """Return (cef_version, archive-entry) for `triple`, asserting filename↔version coherence.

    The archive filename MUST carry the pinned ``cef_version`` — a mismatch means the pin block was
    edited inconsistently (a version bumped in one place but not the file name), which would fetch a
    DIFFERENT build than the manifest claims. We refuse it before anything is downloaded.
    """
    version = manifest.get("cef_version")
    if not version:
        raise FetchError("manifest missing 'cef_version'")
    archives = manifest.get("archives", {})
    if triple not in archives:
        raise FetchError(
            f"unknown triple '{triple}'; manifest has: {', '.join(sorted(archives))}")
    archive = archives[triple]
    for key in ("file", "sha256"):
        if not archive.get(key):
            raise FetchError(f"archive '{triple}' missing '{key}'")
    if version not in archive["file"]:
        raise FetchError(
            f"version-coherence FAILED: archive file '{archive['file']}' does not carry "
            f"cef_version '{version}'")
    return version, archive


def _download_with_retry(url: str, dest: Path, *, attempts: int = _MAX_ATTEMPTS,
                         base_delay: float = _BASE_DELAY, sleep=time.sleep) -> None:
    """Download `url` to `dest` over TLS, retrying transient network failures with exponential
    backoff (Context-Engine#129). Raises FetchError only after `attempts` failures. The SHA-256 pin
    is checked by the caller afterwards, so this resilience never bypasses verification."""
    last_exc: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as resp, dest.open("wb") as out:  # noqa: S310
                shutil.copyfileobj(resp, out)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            last_exc = exc
            if attempt < attempts:
                delay = base_delay * (3 ** (attempt - 1))
                print(f"[fetch_cef] download attempt {attempt}/{attempts} of {url} failed "
                      f"({exc}); retrying in {delay:.0f}s", file=sys.stderr)
                sleep(delay)
    raise FetchError(f"download of {url} failed after {attempts} attempts: {last_exc}") from last_exc


def _obtain(name: str, url: str, source: Path | None, work: Path) -> Path:
    """Return a local path to the archive `name`, from --source or by TLS download."""
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
    if actual.lower() != expected_sha.lower():
        raise VerifyError(
            f"SHA-256 mismatch for {what}: expected {expected_sha}, got {actual} — REFUSED")


def _safe_relname(name: str, prefix: str) -> str | None:
    """Strip the single top-level `prefix/` from a tar member name and guard path traversal.

    Returns the prefix-stripped relative path, or None to skip (the prefix dir itself, or anything
    outside the prefix / attempting to escape via an absolute or '..' component). The archive is
    SHA-256-verified BEFORE extraction, so this is defense-in-depth over a trusted input."""
    p = prefix.rstrip("/") + "/"
    if name == prefix or name == p.rstrip("/"):
        return None
    if not name.startswith(p):
        return None
    rel = name[len(p):]
    if not rel or rel.startswith("/") or ".." in Path(rel).parts:
        return None
    return rel


def _extract(archive: Path, prefix: str, dest: Path) -> int:
    """Extract <prefix>/* from the .tar.bz2 into dest (prefix stripped). Preserves symlinks + modes
    (the macOS framework needs both). Returns the number of members written."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    count = 0
    with tarfile.open(archive, "r:bz2") as tar:
        for member in tar.getmembers():
            rel = _safe_relname(member.name, prefix)
            if rel is None:
                continue
            # Guard symlink / hardlink targets from escaping the destination (defense-in-depth).
            if member.issym() or member.islnk():
                link = member.linkname
                if link.startswith("/") or ".." in Path(link).parts:
                    continue
            member.name = rel
            # filter="tar" is explicit on purpose: it strips absolute/'..'-escaping link targets
            # (defense-in-depth alongside _safe_relname) while PRESERVING file modes — the macOS
            # framework's executable bits. Python 3.12/3.13 deprecate a filterless extract, and
            # Python 3.14 flips the implicit default to the "data" filter, which SANITIZES modes
            # (drops setuid/setgid/sticky + group/other-write) and would silently violate the
            # "modes preserved" contract above. Pinning "tar" makes extraction deterministic across
            # 3.12–3.14+ (all CI interpreters are >= 3.12, where the `filter` kwarg exists).
            tar.extract(member, dest, set_attrs=True, filter="tar")
            count += 1
    if count == 0:
        raise FetchError(
            f"CEF archive carried no files under '{prefix}' — distribution layout changed?")
    return count


def fetch(manifest_path: Path, triple: str, dest: Path,
          source: Path | None = None) -> dict:
    """Download/verify/stage the pinned CEF prebuilt for `triple` into `dest`.

    Returns a dict describing the staged layout. Raises FetchError (config) / VerifyError
    (fail-closed SHA refusal). Idempotent: a matching stamp + present outputs short-circuit.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot read manifest {manifest_path}: {exc}") from exc

    version, archive = resolve(manifest, triple)
    prefix = archive["file"][: -len(".tar.bz2")] if archive["file"].endswith(".tar.bz2") \
        else Path(archive["file"]).stem

    stamp_path = dest / ".cef-fetch-stamp.json"
    sentinel = dest / _ROOT_SENTINEL
    want_stamp = {
        "cef_version": version,
        "triple": triple,
        "sha256": archive["sha256"].lower(),
    }

    if stamp_path.is_file() and sentinel.is_file():
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"root": str(dest), "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ceffetch-") as tmp:
        work = Path(tmp)
        url = _url_for(manifest, archive)
        local = _obtain(archive["file"], url, source, work)
        _verify(local, archive["sha256"], f"CEF distribution ({triple})")
        n = _extract(local, prefix, dest)

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"root": str(dest), "version": version, "members": n, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch + verify the pinned CEF prebuilt.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/cef-prebuilt.json)")
    parser.add_argument("--triple", required=True,
                        help="target triple, e.g. x86_64-unknown-linux-gnu")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging root; the CEF distribution is extracted here (prefix stripped)")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the archive from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.triple, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_cef] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_cef] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    print(f"[fetch_cef] {state} CEF {result['version']} for {args.triple}: root={result['root']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
