#!/usr/bin/env python3
"""Install + verify the pinned naga-cli for the SPIR-V->WGSL cross-compile leg.

Issue #133 (sub-task D of #119) / R-REND-005 — tool ruling: ``docs/wgsl-tool-decision.md``.
This is the acquisition half of the WGSL leg: the flag-gated
``CONTEXT_BUILD_SHADER_CROSSCOMPILE`` backend (``src/render/shadercc/``) invokes ``naga``
as a SUBPROCESS (never linked into the engine — the esbuild / ``tools/fetch_esbuild.py``
precedent), so the tool must exist on the build host before the CMake configure.

Channel — stated precisely, because it is DIFFERENT from the prebuilt fetchers:
naga-cli publishes NO official prebuilt binaries, so this installs FROM SOURCE via
``cargo install --locked`` against crates.io. That is the L-42-PREFERRED from-source
acquisition (the same posture as the vcpkg manifest feature, NOT the third-party-prebuilt
carve-out): cargo itself enforces the crates.io registry checksums for every crate in the
locked graph, so no SHA-pin sidecar is needed here. The pin manifest
(``tools/naga-toolchain.json``) is the SINGLE source of truth for the version — it is also
read by ``src/render/shadercc/CMakeLists.txt`` and baked into the backend as the
fail-closed runtime ``naga --version`` expectation.

What it does, per invocation:
  1. Loads the pin manifest (crate name, exact version, binary name).
  2. If ``<dest>/bin/<binary>`` exists AND ``--version`` reports exactly the pin, exits 0
     (idempotent no-op — the CI cache-restore path, which needs no cargo at all).
  3. Otherwise runs ``cargo install --locked <crate> --version <pin> --root <dest>``.
  4. VERIFIES (fail-closed): runs the installed binary's ``--version`` and requires the
     output to match the pinned version exactly. A mismatch removes nothing but exits 1.

Exit codes (mirrors tools/fetch_esbuild.py / tools/fetch_v8.py):
  * 0 — staged; the binary verified against its pin.
  * 1 — verification FAILED (installed binary does not report the pinned version).
  * 2 — configuration / usage error (bad manifest, cargo missing, cargo install failed).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "naga-toolchain.json"


class FetchError(Exception):
    """Raised for a configuration/usage problem (maps to exit 2)."""


class VerifyError(Exception):
    """Raised when the installed binary fails its version pin (maps to exit 1)."""


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FetchError(f"cannot load pin manifest {path}: {exc}") from exc
    for key in ("crate", "version", "binary"):
        if not manifest.get(key):
            raise FetchError(f"pin manifest {path} missing '{key}'")
    return manifest


def binary_path(dest: Path, manifest: dict) -> Path:
    name = manifest["binary"] + (".exe" if sys.platform == "win32" else "")
    return dest / "bin" / name


def reported_version(binary: Path) -> str | None:
    """The version the installed binary self-reports, or None if it cannot run."""
    try:
        proc = subprocess.run([str(binary), "--version"], capture_output=True,
                              text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return (proc.stdout or proc.stderr).strip()


def verify(binary: Path, pin: str) -> None:
    """Fail-closed: the installed binary MUST self-report exactly the pinned version."""
    version = reported_version(binary)
    if version != pin:
        raise VerifyError(
            f"verification FAILED: {binary} --version reported {version!r}, pin is {pin!r}")


def install(manifest: dict, dest: Path, cargo: str) -> None:
    if shutil.which(cargo) is None:
        raise FetchError(
            f"'{cargo}' not found — install a Rust toolchain (rustup) or restore the tool cache")
    cmd = [cargo, "install", "--locked", manifest["crate"],
           "--version", manifest["version"], "--root", str(dest)]
    print(f"[fetch_naga] $ {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise FetchError(f"cargo install failed with exit code {proc.returncode}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--dest", type=Path, required=True,
                    help="install root; the binary lands at <dest>/bin/")
    ap.add_argument("--cargo", default="cargo", help="cargo executable (default: PATH)")
    args = ap.parse_args()

    try:
        manifest = load_manifest(args.manifest)
        binary = binary_path(args.dest, manifest)
        if binary.is_file() and reported_version(binary) == manifest["version"]:
            print(f"[fetch_naga] up to date: {binary} == {manifest['version']} (no-op)")
            return 0
        install(manifest, args.dest, args.cargo)
        verify(binary, manifest["version"])
    except VerifyError as exc:
        print(f"[fetch_naga] {exc}", file=sys.stderr)
        return 1
    except FetchError as exc:
        print(f"[fetch_naga] ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"[fetch_naga] staged {manifest['crate']} {manifest['version']} at {binary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
