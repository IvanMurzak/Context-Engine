#!/usr/bin/env python3
"""Build + verify + stage the pinned Tint for the SPIR-V->WGSL cross-compile leg.

Issue #133 (sub-task D of #119) / R-REND-005 — measured tool ruling:
``docs/wgsl-tool-decision.md``. This is the acquisition half of the WGSL leg: the
flag-gated ``CONTEXT_BUILD_SHADER_CROSSCOMPILE`` backend (``src/render/shadercc/``)
invokes ``tint`` as a SUBPROCESS (never linked into the engine — the esbuild /
``tools/fetch_esbuild.py`` precedent).

Channel — stated precisely: Tint publishes NO official standalone prebuilts (Dawn's
GitHub releases carry only the emdawnwebgpu web bindings), so this builds FROM SOURCE at
a pinned Dawn release tag — the L-42-PREFERRED acquisition. Provenance is publisher TLS
(github.com/google/dawn, Google's official mirror of dawn.googlesource.com) and the
resolved clone HEAD is verified against the pinned COMMIT hash before anything is built
(fail-closed — a re-pointed or tampered tag is refused). The pin manifest
(``tools/tint-toolchain.json``) is the SINGLE source of truth — it is also read by
``src/render/shadercc/CMakeLists.txt``, which bakes the tag into every compiled artifact
(``wgsltool=`` line) because tint has no ``--version`` self-report.

What it does, per invocation:
  1. Loads the pin manifest (repository, tag, commit, cmake args, build target).
  2. If the stamp (``<dest>/tint-stamp.json``) matches the pin AND the staged binary
     passes the functional smoke, exits 0 (idempotent no-op — the CI cache-restore path,
     which needs neither git nor a compiler).
  3. Otherwise: shallow-clones the tag, VERIFIES ``rev-parse HEAD`` == the pinned commit
     (fail-closed), configures + builds the ``tint`` CLI target only, and stages the
     binary to ``<dest>/bin/``.
  4. Functional smoke (fail-closed): the staged binary must round-trip a one-line WGSL
     module (``tint --help`` exits 1 by design, so a real translation is the smoke).
  5. Writes the stamp. The clone/build tree lives in a temp dir and is deleted, so a CI
     cache of ``<dest>`` stays binary-sized.

Exit codes (mirrors tools/fetch_esbuild.py / tools/fetch_naga.py):
  * 0 — staged; the source verified against its pin and the binary passed the smoke.
  * 1 — verification FAILED (commit mismatch, or the staged binary fails the smoke).
  * 2 — configuration / usage error (bad manifest, git/cmake missing or failing).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "tint-toolchain.json"

MANIFEST_KEYS = ("repository", "tag", "commit", "target", "binary", "cmake_args")


class FetchError(Exception):
    """Raised for a configuration/usage problem (maps to exit 2)."""


class VerifyError(Exception):
    """Raised when the pinned commit or the staged binary fails verification (maps to exit 1)."""


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot load pin manifest {path}: {exc}") from exc
    for key in MANIFEST_KEYS:
        if not manifest.get(key):
            raise FetchError(f"pin manifest {path} missing '{key}'")
    if len(manifest["commit"]) != 40:
        raise FetchError(f"pin manifest {path}: 'commit' must be a full 40-hex sha")
    return manifest


def binary_path(dest: Path, manifest: dict) -> Path:
    name = manifest["binary"] + (".exe" if sys.platform == "win32" else "")
    return dest / "bin" / name


def stamp_path(dest: Path) -> Path:
    return dest / "tint-stamp.json"


def stamp_matches(dest: Path, manifest: dict) -> bool:
    try:
        stamp = json.loads(stamp_path(dest).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return stamp.get("tag") == manifest["tag"] and stamp.get("commit") == manifest["commit"]


def run(cmd: list[str], **kwargs) -> None:
    print(f"[fetch_tint] $ {' '.join(str(c) for c in cmd)}", flush=True)
    proc = subprocess.run(cmd, **kwargs)
    if proc.returncode != 0:
        raise FetchError(f"'{cmd[0]}' failed with exit code {proc.returncode}")


def smoke(binary: Path) -> None:
    """Fail-closed functional smoke: the staged tint must round-trip a trivial WGSL module."""
    with tempfile.TemporaryDirectory(prefix="tint-smoke-") as td:
        src = Path(td) / "smoke.wgsl"
        out = Path(td) / "out.wgsl"
        src.write_text("fn smoke() {}\n", encoding="utf-8")
        try:
            proc = subprocess.run([str(binary), "--format", "wgsl", "-o", str(out), str(src)],
                                  capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise VerifyError(f"smoke FAILED: cannot run {binary}: {exc}") from exc
        if proc.returncode != 0 or not out.is_file():
            raise VerifyError(f"smoke FAILED: {binary} exited {proc.returncode}: "
                              f"{(proc.stderr or proc.stdout).strip()}")


def clone_and_verify(manifest: dict, work: Path) -> Path:
    """Shallow-clone the pinned tag and verify its HEAD against the pinned commit (fail-closed).

    The clone is retried with backoff (Context-Engine#132 posture: transient CDN/network failures
    must not hard-fail CI); the commit verification runs AFTER, so retrying never weakens the pin.
    """
    src = work / "dawn"
    last: FetchError | None = None
    for attempt in range(1, 4):
        try:
            run(["git", "clone", "--depth", "1", "--branch", manifest["tag"],
                 manifest["repository"], str(src)])
            last = None
            break
        except FetchError as exc:
            last = exc
            shutil.rmtree(src, ignore_errors=True)
            if attempt < 3:
                delay = 3.0 * (3 ** (attempt - 1))
                print(f"[fetch_tint] clone attempt {attempt}/3 failed ({exc}); "
                      f"retrying in {delay:.0f}s", file=sys.stderr)
                time.sleep(delay)
    if last is not None:
        raise last
    proc = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"],
                          capture_output=True, text=True)
    head = proc.stdout.strip()
    if proc.returncode != 0 or head != manifest["commit"]:
        raise VerifyError(f"verification FAILED: tag {manifest['tag']} resolved to "
                          f"{head!r}, pin is {manifest['commit']!r} — refusing to build")
    return src


def build_and_stage(manifest: dict, src: Path, work: Path, dest: Path) -> Path:
    build = work / "build"
    run(["cmake", "-S", str(src), "-B", str(build), *manifest["cmake_args"]])
    run(["cmake", "--build", str(build), "--target", manifest["target"],
         "--config", "Release", "--parallel"])

    exe = manifest["binary"] + (".exe" if sys.platform == "win32" else "")
    candidates = [p for p in build.rglob(exe) if p.is_file()]
    if not candidates:
        raise FetchError(f"build succeeded but no '{exe}' found under {build}")
    built = min(candidates, key=lambda p: len(p.parts))  # the CLI lands nearest the build root

    staged = binary_path(dest, manifest)
    staged.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, staged)
    if sys.platform != "win32":
        staged.chmod(staged.stat().st_mode | 0o755)
    return staged


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--dest", type=Path, required=True,
                    help="stage root; the binary lands at <dest>/bin/")
    args = ap.parse_args()

    try:
        manifest = load_manifest(args.manifest)
        binary = binary_path(args.dest, manifest)
        if binary.is_file() and stamp_matches(args.dest, manifest):
            smoke(binary)
            print(f"[fetch_tint] up to date: {binary} @ {manifest['tag']} (no-op)")
            return 0
        with tempfile.TemporaryDirectory(prefix="tint-build-") as td:
            work = Path(td)
            src = clone_and_verify(manifest, work)
            staged = build_and_stage(manifest, src, work, args.dest)
        smoke(staged)
        stamp_path(args.dest).write_text(
            json.dumps({"tag": manifest["tag"], "commit": manifest["commit"]}) + "\n",
            encoding="utf-8")
    except VerifyError as exc:
        print(f"[fetch_tint] {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_tint] ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"[fetch_tint] staged tint @ {manifest['tag']} at {binary_path(args.dest, manifest)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
