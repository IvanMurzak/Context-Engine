#!/usr/bin/env python3
"""Fetch + verify + stage the pinned V8 prebuilt for the in-process JS host.

Issue #76 / L-61 / R-LANG-002/008 / R-SEC-009. This is the acquisition half of the
SHA-pinned third-party prebuilt channel (denoland/rusty_v8 static libs + the
version-coherent ``v8-<ver>.crate`` C++ headers). It implements R-SEC-009's
**verify-before-use, fail-closed** posture for a *third-party* build lib — the
wgpu-native / CEF precedent codified in ``docs/signing.md`` (third-party libs are
authenticated by their own publisher via TLS + SHA-pin; they are explicitly OUT of
scope for the engine first-party trust root, so this deliberately does NOT route
through ``tools/verify_artifact.py``).

What it does, per invocation:
  1. Loads the pin manifest (``tools/v8-prebuilt.json``) — the single source of truth.
  2. Asserts VERSION-COHERENCE (the critical correctness invariant): the header crate
     name + include prefix must carry the SAME version as ``rusty_v8_version``. A
     lib/header version skew is refused before anything is downloaded.
  3. Downloads the ``<triple>`` static lib (a bare-gzip ``.a.gz`` / ``.lib.gz``) over
     TLS, verifies its SHA-256 against the pin (fail-closed), and gunzips it to
     ``<dest>/lib/<extracted>``.
  4. Downloads the header crate (a gzipped tar) over TLS, verifies its SHA-256, and
     extracts ``<include_prefix>/*`` into ``<dest>/include/`` (prefix stripped).
  5. Writes an idempotency stamp so a re-run with matching pins + present outputs is a
     no-op (the pipeline re-enters this configure step on every CI-fail retry).

Offline / air-gapped / test: ``--source <dir>`` reads the lib + crate from a local
directory (by their manifest file names) instead of the network — the SHA
verification still runs (fail-closed), so the offline path is exactly as safe.

Exit codes (mirrors tools/verify_artifact.py):
  * 0 — staged; every artifact verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, unknown triple, version skew,
        missing --source file, download error).
"""

from __future__ import annotations

import argparse
import gzip
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
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "v8-prebuilt.json"

_CHUNK = 1 << 20

# Retry policy for transient upstream failures (Context-Engine#129): a single GitHub-releases /
# CDN 504 or timeout must not hard-fail the fetch. SHA-256 verification still runs AFTER the
# download completes (fail-closed), so retrying never weakens the pin.
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
    """Assert the header crate is version-locked to ``rusty_v8_version``. Returns the version.

    This is THE correctness invariant the owner ruling flagged: the static lib and the
    C++ headers MUST come from the same V8 — a skew would compile against one ABI and
    link against another. We enforce it structurally from the pin names.
    """
    version = manifest.get("rusty_v8_version")
    if not version:
        raise FetchError("manifest missing 'rusty_v8_version'")
    headers = manifest.get("headers", {})
    crate = headers.get("crate", "")
    prefix = headers.get("include_prefix", "")
    expected_crate = f"v8-{version}.crate"
    expected_prefix = f"v8-{version}/"
    if crate != expected_crate:
        raise FetchError(
            f"version-coherence FAILED: header crate '{crate}' != '{expected_crate}' "
            f"(rusty_v8_version={version})")
    if not prefix.startswith(expected_prefix):
        raise FetchError(
            f"version-coherence FAILED: include_prefix '{prefix}' does not start with "
            f"'{expected_prefix}' (rusty_v8_version={version})")
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
                print(f"[fetch_v8] download attempt {attempt}/{attempts} of {url} failed "
                      f"({exc}); retrying in {delay:.0f}s", file=sys.stderr)
                sleep(delay)
    raise FetchError(f"download of {url} failed after {attempts} attempts: {last_exc}")


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


def _gunzip(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(src, "rb") as gz, dst.open("wb") as out:
        shutil.copyfileobj(gz, out)


def _extract_headers(crate: Path, include_prefix: str, dest_include: Path) -> int:
    """Extract <include_prefix>/* from the crate tar.gz into dest_include (prefix stripped)."""
    if dest_include.exists():
        shutil.rmtree(dest_include)
    dest_include.mkdir(parents=True, exist_ok=True)
    prefix = include_prefix.rstrip("/") + "/"
    count = 0
    with tarfile.open(crate, "r:gz") as tar:
        for member in tar.getmembers():
            if not member.isfile() or not member.name.startswith(prefix):
                continue
            rel = member.name[len(prefix):]
            if not rel or rel.startswith("/") or ".." in Path(rel).parts:
                continue  # path-traversal guard
            target = dest_include / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            extracted = tar.extractfile(member)
            if extracted is None:
                continue
            with target.open("wb") as out:
                shutil.copyfileobj(extracted, out)
            count += 1
    if count == 0:
        raise FetchError(
            f"header crate carried no files under '{include_prefix}' — layout changed?")
    return count


def fetch(manifest_path: Path, triple: str, dest: Path,
          source: Path | None = None) -> dict:
    """Download/verify/stage the pinned V8 prebuilt for `triple` into `dest`.

    Returns a dict describing the staged layout. Raises FetchError (config) / VerifyError
    (fail-closed SHA refusal). Idempotent: a matching stamp + present outputs short-circuit.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot read manifest {manifest_path}: {exc}") from exc

    version = check_coherence(manifest)
    libs = manifest.get("libs", {})
    if triple not in libs:
        raise FetchError(
            f"unknown triple '{triple}'; manifest has: {', '.join(sorted(libs))}")
    lib = libs[triple]
    headers = manifest["headers"]

    lib_out = dest / "lib" / lib["extracted"]
    include_out = dest / "include"
    stamp_path = dest / ".v8-fetch-stamp.json"
    want_stamp = {
        "rusty_v8_version": version,
        "triple": triple,
        "lib_sha256": lib["sha256"],
        "headers_sha256": headers["sha256"],
    }

    if stamp_path.is_file() and lib_out.is_file() and (include_out / "v8-isolate.h").is_file():
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"lib": str(lib_out), "include": str(include_out),
                        "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="v8fetch-") as tmp:
        work = Path(tmp)

        lib_url = manifest["lib_release_url_template"].format(
            rusty_v8_version=version, file=lib["file"])
        lib_gz = _obtain(lib["file"], lib_url, source, work)
        _verify(lib_gz, lib["sha256"], f"static lib ({triple})")
        _gunzip(lib_gz, lib_out)

        crate = _obtain(headers["crate"], headers["url"], source, work)
        _verify(crate, headers["sha256"], "header crate")
        n_headers = _extract_headers(crate, headers["include_prefix"], include_out)

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"lib": str(lib_out), "include": str(include_out), "version": version,
            "headers": n_headers, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch + verify the pinned V8 prebuilt.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/v8-prebuilt.json)")
    parser.add_argument("--triple", required=True,
                        help="target triple, e.g. x86_64-unknown-linux-gnu")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging root; libs land in <dest>/lib, headers in <dest>/include")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the lib + crate from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.triple, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_v8] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_v8] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    print(f"[fetch_v8] {state} V8 {result['version']} for {args.triple}: "
          f"lib={result['lib']} include={result['include']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
