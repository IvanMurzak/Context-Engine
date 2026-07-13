#!/usr/bin/env python3
"""Fetch + verify + stage the pinned wasmtime C-API prebuilt for the sandboxed WASM runner.

Issue #71 / L-37 / L-62 / R-SEC-009. This is the acquisition half of the SHA-pinned
third-party prebuilt channel for the package-migration WASM tier — the DIRECT SIBLING of
``tools/fetch_v8.py``, mirroring its posture 1:1: TLS download + SHA-256 pin +
verify-before-use (fail-closed) + offline ``--source`` + an idempotency stamp + transient-
failure retry/backoff, with the identical 0/1/2 exit-code contract. It implements R-SEC-009's
**verify-before-use, fail-closed** posture for a *third-party* build lib — the
wgpu-native / CEF / rusty_v8 precedent codified in ``docs/signing.md`` (third-party libs are
authenticated by their own publisher via TLS + SHA-pin; they are explicitly OUT of scope for
the engine first-party trust root, so this deliberately does NOT route through
``tools/verify_artifact.py``).

WHERE IT DIFFERS FROM fetch_v8.py — the wasmtime C-API prebuilt ships ONE archive per platform
(a ``.zip`` on Windows / ``.tar.xz`` on linux+macos) carrying BOTH the C headers and the libs
together, rather than V8's separately-fetched gzipped static lib PLUS a version-locked header
crate. So there is a single artifact to obtain + verify, and staging extracts the archive's
``<root>/include`` + ``<root>/lib`` into a flattened ``<dest>/include`` + ``<dest>/lib`` (the
same flattened layout src/runtime/wasm/CMakeLists.txt consumes by convention, mirroring how
src/runtime/js/CMakeLists.txt consumes fetch_v8.py's ``<dest>/lib/<extracted>`` + ``<dest>/include``).

What it does, per invocation:
  1. Loads the pin manifest (``tools/wasmtime-prebuilt.json``) — the single source of truth.
  2. Asserts VERSION-COHERENCE (the critical correctness invariant): every platform's archive
     name must be ``wasmtime-v<wasmtime_version>-<platform>-c-api.<format>``. An archive whose
     embedded version does not match ``wasmtime_version`` is refused before any download.
  3. Downloads the ``<platform>`` C-API archive over TLS, verifies its SHA-256 against the pin
     (fail-closed), and extracts it (zip / tar.xz / tar.gz), path-traversal-guarded.
  4. Stages the archive's ``<root>/include`` -> ``<dest>/include`` and ``<root>/lib`` ->
     ``<dest>/lib``, then asserts the pinned header + lib landed.
  5. Writes an idempotency stamp so a re-run with matching pins + present outputs is a no-op
     (the pipeline re-enters the CMake configure step on every CI-fail retry).

Offline / air-gapped / test: ``--source <dir>`` reads the archive from a local directory (by
its manifest file name) instead of the network — the SHA verification still runs (fail-closed),
so the offline path is exactly as safe.

Exit codes (mirrors tools/fetch_v8.py / tools/verify_artifact.py):
  * 0 — staged; the archive verified against its pin.
  * 1 — a SHA-256 verification FAILED (tampered / wrong file) — the fail-closed refusal.
  * 2 — configuration / usage error (bad manifest, unknown platform, version skew, unknown
        archive format, missing --source file, download error, unexpected archive layout).
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
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "wasmtime-prebuilt.json"

_CHUNK = 1 << 20

# The archive formats the wasmtime release channel ships (a .zip on Windows, .tar.xz on
# linux/macos); .tar.gz is accepted too so a synthetic offline fixture can avoid lzma.
_VALID_FORMATS = ("zip", "tar.xz", "tar.gz")

# Retry policy for transient upstream failures (Context-Engine#129): a single GitHub-releases /
# CDN 504 or timeout must not hard-fail the fetch. SHA-256 verification still runs AFTER the
# download completes (fail-closed), so retrying never weakens the pin. Kept identical to fetch_v8.py.
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
    """Assert every platform's archive is version-locked to ``wasmtime_version``. Returns the version.

    This is THE correctness invariant the owner ruling flagged (the wasmtime analog of
    fetch_v8's header-crate version-lock): the pinned archive MUST carry the same wasmtime as
    ``wasmtime_version`` — a skew would fetch a different runtime than the pins document. We
    enforce it structurally from the archive name (``wasmtime-v<ver>-<platform>-c-api.<fmt>``).
    """
    version = manifest.get("wasmtime_version")
    if not version:
        raise FetchError("manifest missing 'wasmtime_version'")
    platforms = manifest.get("platforms", {})
    if not platforms:
        raise FetchError("manifest missing 'platforms'")
    for plat, entry in platforms.items():
        fmt = entry.get("format", "")
        if fmt not in _VALID_FORMATS:
            raise FetchError(
                f"platform '{plat}': unknown archive format '{fmt}' "
                f"(expected one of {', '.join(_VALID_FORMATS)})")
        archive = entry.get("archive", "")
        expected = f"wasmtime-v{version}-{plat}-c-api.{fmt}"
        if archive != expected:
            raise FetchError(
                f"version-coherence FAILED: platform '{plat}' archive '{archive}' != "
                f"'{expected}' (wasmtime_version={version})")
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
                print(f"[fetch_wasmtime] download attempt {attempt}/{attempts} of {url} failed "
                      f"({exc}); retrying in {delay:.0f}s", file=sys.stderr)
                sleep(delay)
    raise FetchError(
        f"download of {url} failed after {attempts} attempts: {last_exc}") from last_exc


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


def _safe_rel(rel: str) -> bool:
    """Path-traversal guard for an archive member (mirrors fetch_v8._extract_headers)."""
    if not rel or rel.startswith("/") or rel.startswith("\\"):
        return False
    return ".." not in Path(rel).parts


def _extract_archive(archive: Path, fmt: str, dest: Path) -> int:
    """Extract every file from the C-API archive into `dest`, preserving internal structure.

    Streamed member-by-member (the static lib is large) and path-traversal-guarded. Returns
    the number of files extracted. `dest` gains the archive's own ``<root>/`` top dir.
    """
    dest.mkdir(parents=True, exist_ok=True)
    count = 0
    if fmt == "zip":
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if info.is_dir() or not _safe_rel(info.filename):
                    continue
                target = dest / info.filename
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, target.open("wb") as out:
                    shutil.copyfileobj(src, out)
                count += 1
    elif fmt in ("tar.xz", "tar.gz"):
        mode = "r:xz" if fmt == "tar.xz" else "r:gz"
        with tarfile.open(archive, mode) as tar:
            for member in tar.getmembers():
                if not member.isfile() or not _safe_rel(member.name):
                    continue
                extracted = tar.extractfile(member)
                if extracted is None:
                    continue
                target = dest / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("wb") as out:
                    shutil.copyfileobj(extracted, out)
                count += 1
    else:
        raise FetchError(
            f"unknown archive format '{fmt}' (expected one of {', '.join(_VALID_FORMATS)})")
    if count == 0:
        raise FetchError(f"archive {archive.name} carried no files — layout changed?")
    return count


def _stage_tree(src_dir: Path, dst_dir: Path) -> None:
    """Replace `dst_dir` with a fresh copy of `src_dir` (idempotent re-stage)."""
    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    shutil.copytree(src_dir, dst_dir)


def fetch(manifest_path: Path, platform: str, dest: Path,
          source: Path | None = None) -> dict:
    """Download/verify/stage the pinned wasmtime C-API prebuilt for `platform` into `dest`.

    Returns a dict describing the staged layout. Raises FetchError (config) / VerifyError
    (fail-closed SHA refusal). Idempotent: a matching stamp + present outputs short-circuit.
    """
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot read manifest {manifest_path}: {exc}") from exc

    version = check_coherence(manifest)
    platforms = manifest.get("platforms", {})
    if platform not in platforms:
        raise FetchError(
            f"unknown platform '{platform}'; manifest has: {', '.join(sorted(platforms))}")
    entry = platforms[platform]
    header_rel = manifest.get("header", "wasmtime.h")

    root_name = f"wasmtime-v{version}-{platform}-c-api"  # coherence-validated above
    include_out = dest / "include"
    lib_out_dir = dest / "lib"
    header_out = include_out / header_rel
    lib_out = lib_out_dir / entry["lib"]
    stamp_path = dest / ".wasmtime-fetch-stamp.json"
    want_stamp = {
        "wasmtime_version": version,
        "platform": platform,
        "archive_sha256": entry["sha256"],
    }

    if stamp_path.is_file() and header_out.is_file() and lib_out.is_file():
        try:
            if json.loads(stamp_path.read_text(encoding="utf-8")) == want_stamp:
                return {"lib": str(lib_out), "include": str(include_out),
                        "version": version, "cached": True}
        except (OSError, json.JSONDecodeError):
            pass  # corrupt stamp -> re-stage

    dest.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wasmtimefetch-") as tmp:
        work = Path(tmp)
        archive_url = manifest["release_url_template"].format(
            wasmtime_version=version, archive=entry["archive"])
        archive = _obtain(entry["archive"], archive_url, source, work)
        _verify(archive, entry["sha256"], f"C-API archive ({platform})")

        extract_root = work / "extracted"
        n_files = _extract_archive(archive, entry["format"], extract_root)
        src_root = extract_root / root_name
        if not (src_root / "include").is_dir() or not (src_root / "lib").is_dir():
            raise FetchError(
                f"extracted archive has no '{root_name}/include' + '{root_name}/lib' — "
                f"wasmtime C-API layout changed?")
        _stage_tree(src_root / "include", include_out)
        _stage_tree(src_root / "lib", lib_out_dir)

    if not header_out.is_file():
        raise FetchError(f"staged C-API is missing header '{header_rel}' (expected {header_out})")
    if not lib_out.is_file():
        raise FetchError(
            f"staged C-API is missing lib '{entry['lib']}' for {platform} (expected {lib_out})")

    stamp_path.write_text(json.dumps(want_stamp, indent=2) + "\n", encoding="utf-8")
    return {"lib": str(lib_out), "include": str(include_out), "version": version,
            "files": n_files, "cached": False}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch + verify the pinned wasmtime C-API prebuilt.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="pin manifest (default: tools/wasmtime-prebuilt.json)")
    parser.add_argument("--platform", required=True,
                        help="target platform, e.g. x86_64-linux / aarch64-macos / x86_64-windows")
    parser.add_argument("--dest", type=Path, required=True,
                        help="staging root; libs land in <dest>/lib, headers in <dest>/include")
    parser.add_argument("--source", type=Path, default=None,
                        help="offline: read the C-API archive from this local dir instead of TLS")
    args = parser.parse_args(argv)

    try:
        result = fetch(args.manifest, args.platform, args.dest, args.source)
    except VerifyError as exc:
        print(f"[fetch_wasmtime] REFUSED: {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_wasmtime] ERROR: {exc}", file=sys.stderr)
        return 2

    state = "cached" if result.get("cached") else "staged"
    print(f"[fetch_wasmtime] {state} wasmtime {result['version']} for {args.platform}: "
          f"lib={result['lib']} include={result['include']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
