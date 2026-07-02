#!/usr/bin/env python3
"""Rebuilds spikes/wasm/guest/module.wasm from module.cpp with a wasm32-capable clang.

The compiled artifact is COMMITTED (module.wasm, ~2 KB) so building/running the spike — locally
and in CI on all three platforms — needs NO wasm toolchain; only rebuilding the guest does.
This mirrors the js-engine spike's prebuilt-for-throwaway-spikes precedent (recorded L-42
deviation; see FINDINGS.md §"Deviations").

Toolchain used for the committed artifact (recorded acquisition route):
    wasi-sdk 33.0 (clang 22.1.0), x86_64-windows tarball from
    https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-33
    sha256: df14ca2a2127c2d6b6be07e6f5549b3af9c1b3c0112430c200a4749970c59f06
    (only clang/wasm-ld are used; the module targets wasm32-unknown-unknown FREESTANDING —
    no wasi-libc, no WASI imports, no imports at all.)

Usage:
    python build_module.py [--clang <path-to-clang++>]
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

FLAGS = [
    "--target=wasm32-unknown-unknown",
    "-O2",
    "-nostdlib",
    "-fno-builtin",          # keep clang from synthesizing memset/memcpy libcalls
    "-fvisibility=hidden",   # exports are opted in via export_name only
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wl,--no-entry",        # reactor-style module: no _start
    "-Wl,-z,stack-size=65536",
    "-Wl,--max-memory=1073741824",  # 16 GiB would be silly; 1 GiB cap, leaves room for mem_grow
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", default=None,
                        help="path to a wasm32-capable clang++ (default: clang++ on PATH)")
    args = parser.parse_args()

    clang = args.clang or shutil.which("clang++") or shutil.which("clang")
    if not clang:
        print("ERROR: no clang++ found; pass --clang <path> (e.g. <wasi-sdk>/bin/clang++)")
        return 2

    src = HERE / "module.cpp"
    out = HERE / "module.wasm"
    cmd = [clang, *FLAGS, str(src), "-o", str(out)]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)

    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    print(f"built {out.name}: {out.stat().st_size} bytes, sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
