#!/usr/bin/env python3
"""L-8/R-LANG-003 spike driver: runs context-spike-wasm across runtime configs in FRESH
processes, aggregates the numbers, and writes results.json.

Fresh processes matter: footprint/memory floors must not contaminate each other, and each
runtime config initializes global state (WAMR's runtime singleton, wasmtime's engine).

Usage:  python run_bench.py --exe <path-to-context-spike-wasm> [--out results.json]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import platform
import subprocess
import sys
from pathlib import Path

RUNTIMES = ["wasmtime", "wasmtime-winch", "wasmtime-pulley", "wamr", "wamr-aot"]
BENCHES = ["load", "calls", "zerocopy", "compute", "trap", "grow", "memory"]


def hardware_info() -> dict:
    info = {
        "os": f"{platform.system()} {platform.release()} ({platform.version()})",
        "machine": platform.machine(),
    }
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            info["cpu"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    except Exception:
        info["cpu"] = platform.processor()
    try:
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        ms = MEMORYSTATUSEX()
        ms.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
        info["ram_gb"] = round(ms.ullTotalPhys / (1024 ** 3), 1)
    except Exception:
        pass
    return info


def run_bench(exe: Path, runtime: str, bench: str) -> dict:
    cmd = [str(exe), f"--runtime={runtime}", f"--bench={bench}"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if proc.returncode != 0:
        return {"_failed": True, "_exit": proc.returncode,
                "_stderr": proc.stderr.strip()[-500:]}
    for line in reversed(proc.stdout.strip().splitlines()):
        if line.startswith("{"):
            return json.loads(line)
    return {"_failed": True, "_exit": 0, "_stderr": "no JSON in stdout"}


def binary_footprint(exe: Path) -> dict:
    """Sizes (MB) of everything the spike stages next to the exe."""
    files = {}
    for f in sorted(exe.parent.iterdir()):
        if f.suffix.lower() in (".exe", ".dll", ".wasm", ".aot", ".so", ".dylib") or \
                f.name == exe.name:
            files[f.name] = round(f.stat().st_size / (1024 * 1024), 3)
    return files


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # Windows consoles default to cp1252
    except Exception:
        pass

    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--out", default="results.json")
    args = parser.parse_args()
    exe = Path(args.exe).resolve()

    results: dict = {
        "hardware": hardware_info(),
        "binary_footprint_mb": binary_footprint(exe),
        "runtimes": {},
    }

    for runtime in RUNTIMES:
        results["runtimes"][runtime] = {}
        for bench in BENCHES:
            print(f"[run_bench] {runtime} / {bench} ...", flush=True)
            results["runtimes"][runtime][bench] = run_bench(exe, runtime, bench)

    out = Path(args.out)
    out.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"[run_bench] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
