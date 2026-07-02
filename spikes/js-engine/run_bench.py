#!/usr/bin/env python3
"""§2d spike driver: runs context-spike-jsengine across engine configs in FRESH processes,
aggregates median-of-5 numbers, and writes results.json + a markdown summary.

Fresh processes matter: V8 flags (--jitless) are process-wide, startup must be process-cold,
and memory floors must not contaminate each other.

Usage:  python run_bench.py --exe <path-to-context-spike-jsengine> [--out results.json]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import platform
import statistics
import subprocess
import sys
from pathlib import Path

CONFIGS = [
    ("quickjs", []),
    ("v8", []),
    ("v8-jitless", ["--jitless"]),
]
PER_PROCESS_BENCHES = ["calls", "zerocopy", "compute", "gc"]  # internal median-of-5
COLD_REPS = 5  # startup + memory get 5 fresh processes each


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


def run_bench(exe: Path, engine_flag: str, extra: list[str], bench: str) -> dict | None:
    """Runs one bench in a fresh process; returns the parsed JSON line or None on failure."""
    cmd = [str(exe), f"--engine={engine_flag}", *extra, f"--bench={bench}"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        return {"_failed": True, "_exit": proc.returncode,
                "_stderr": proc.stderr.strip()[-500:]}
    # V8 may print flag warnings before the JSON line — take the last '{' line.
    for line in reversed(proc.stdout.strip().splitlines()):
        if line.startswith("{"):
            return json.loads(line)
    return {"_failed": True, "_exit": 0, "_stderr": "no JSON in stdout"}


def med(values: list[float]) -> dict:
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def binary_footprint(exe: Path) -> dict:
    files = {}
    for f in sorted(exe.parent.iterdir()):
        if f.suffix.lower() in (".exe", ".dll", ".dat"):
            files[f.name] = round(f.stat().st_size / (1024 * 1024), 2)
    return files


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # Windows consoles default to cp1252
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", default="results.json")
    args = ap.parse_args()
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        print(f"exe not found: {exe}", file=sys.stderr)
        return 1

    results = {"hardware": hardware_info(), "binary_footprint_mb": binary_footprint(exe),
               "configs": {}}
    print(f"hardware: {results['hardware']}")

    for name, extra in CONFIGS:
        engine_flag = "v8" if name.startswith("v8") else name
        cfg: dict = {}
        print(f"\n=== {name} ===")

        startups, priv_deltas, ws_deltas = [], [], []
        for _ in range(COLD_REPS):
            r = run_bench(exe, engine_flag, extra, "startup")
            if r and not r.get("_failed"):
                startups.append(r["startup_to_first_eval_ms"])
                cfg["version"] = r["version"]
            r = run_bench(exe, engine_flag, extra, "memory")
            if r and not r.get("_failed"):
                priv_deltas.append(r["memory"]["private_delta_mb"])
                ws_deltas.append(r["memory"]["working_set_delta_mb"])
        if startups:
            cfg["startup_to_first_eval_ms"] = med(startups)
            print(f"  startup-to-first-eval ms: {cfg['startup_to_first_eval_ms']}")
        if priv_deltas:
            cfg["memory_floor_private_mb"] = med(priv_deltas)
            cfg["memory_floor_working_set_mb"] = med(ws_deltas)
            print(f"  memory floor (private MB): {cfg['memory_floor_private_mb']}")

        for bench in PER_PROCESS_BENCHES:
            r = run_bench(exe, engine_flag, extra, bench)
            if r is None or r.get("_failed"):
                cfg[bench] = {"failed": True, "detail": r}
                print(f"  {bench}: FAILED {r.get('_stderr', '') if r else ''}")
            else:
                cfg[bench] = r[bench]
                print(f"  {bench}: ok")

        results["configs"][name] = cfg

    out = Path(args.out)
    out.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"\nwrote {out}")

    # -- compact summary table ------------------------------------------------------------
    def g(cfg_name: str, *path, default="—"):
        node = results["configs"].get(cfg_name, {})
        for p in path:
            if not isinstance(node, dict) or p not in node:
                return default
            node = node[p]
        return node

    rows = [
        ("startup→first eval (ms)", lambda c: f"{g(c, 'startup_to_first_eval_ms', 'median'):.3g}"),
        ("memory floor, private (MB)", lambda c: f"{g(c, 'memory_floor_private_mb', 'median'):.3g}"),
        ("host→JS empty (Mcalls/s)", lambda c: fmt_m(g(c, 'calls', 'host_to_js_empty', 'ops_per_sec', 'median'))),
        ("JS→host empty (Mcalls/s)", lambda c: fmt_m(g(c, 'calls', 'js_to_host_empty', 'ops_per_sec', 'median'))),
        ("detach ns (vm-shared)", lambda c: f"{g(c, 'zerocopy', 'vm_allocated_shared', 'detach_ns', 'median'):.3g}"),
        ("compute kernel (runs/s)", lambda c: f"{g(c, 'compute', 'kernel_runs_per_sec', 'median'):.4g}"),
        ("alloc-loop max gap (ms)", lambda c: f"{g(c, 'gc', 'alloc_loop_max_gap_ms', 'median'):.3g}"),
    ]

    def fmt_m(v):
        return f"{v / 1e6:.3g}" if isinstance(v, (int, float)) else "—"

    names = [n for n, _ in CONFIGS]
    print("\n| metric | " + " | ".join(names) + " |")
    print("|---|" + "---|" * len(names))
    for label, fn in rows:
        cells = []
        for n in names:
            try:
                cells.append(fn(n))
            except (TypeError, ValueError):
                cells.append("—")
        print(f"| {label} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
