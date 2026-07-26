#!/usr/bin/env python3
"""Rate-measure a CEF smoke executable: does it EXIT, and if not, had it already passed? (issue #437)

A CEF smoke has a failure mode that a single run cannot classify and that ctest reports only as
`***Timeout`: the executable does all its work, prints its whole success verdict, and then never
returns from `CefShutdown()`. Read as a timeout it looks like a slow or stuck scenario, and the
reflex fix — widen `TIMEOUT` — cannot work, because the scenario already finished. #437 was
misdiagnosed twice off single observations, so this tool exists to answer two questions no single
probe can:

  1. **PASS or HANG — at what RATE?** State-dependent hangs are the norm for this class (#437's own
     ON/OFF switch was a machine-global keychain item created between two measurements), so a lone
     green run proves nothing. `-k` runs each executable K times and prints the rate.
  2. **Did it hang BEFORE or AFTER its verdict?** `HUNG_AFTER_VERDICT` (a teardown wedge — the work
     succeeded) and `HUNG_NO_VERDICT` (a real stall in the scenario) have nothing in common except
     the exit status ctest shows, and they lead to opposite investigations.

Verdicts: `PASS` · `FAIL(rc=N)` · `HUNG_AFTER_VERDICT` · `HUNG_NO_VERDICT`.

A hung run is diagnosed, not just reported: with `sample(1)` available (macOS) the tool captures the
blocked stacks, and it always records any pending macOS `SecurityAgent` process, because a keychain
authorization prompt nothing can answer is the known cause (#437) and it is invisible in the
executable's own output.

Exit 0 = every run of every executable PASSed. Exit 1 = at least one did not (the point of a rate
tool is that this is a JUDGEMENT, so the summary is always printed either way). Exit 2 = the tool
could not run at all (no executable matched), which is deliberately NOT a pass.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

# The CEF smoke executables, as (ctest name, path GLOB relative to the build dir). macOS builds each
# into an .app bundle, every other platform emits a bare executable, so both shapes are globbed.
# Keep this list in step with the `cef-substrate-*` / `editor-cef-smoke-*` ctest registrations.
SMOKES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("cef-substrate-boot", ("editor/cef/**/context_cef_boot_smoke",)),
    ("editor-cef-smoke-boot", ("editor/gui/host/**/context_gui_host",)),
    ("editor-cef-smoke-shell", ("editor/shell/**/context_editor_shell_cef_smoke",)),
    ("editor-cef-smoke-shell-restore", ("editor/shell/**/context_editor_shell_restore_smoke",)),
    ("editor-cef-smoke-shell-palette", ("editor/shell/**/context_editor_shell_palette_smoke",)),
    ("editor-cef-smoke-shell-multiwindow",
     ("editor/shell/**/context_editor_shell_multiwindow_smoke",)),
    ("editor-cef-smoke-shell-tearout", ("editor/shell/**/context_editor_shell_tearout_smoke",)),
    ("editor-cef-smoke-shell-drag", ("editor/shell/**/context_editor_shell_drag_smoke",)),
    ("editor-cef-smoke-shell-iframe", ("editor/shell/**/context_editor_shell_iframe_smoke",)),
    ("editor-cef-smoke-shell-settings", ("editor/shell/**/context_editor_shell_settings_smoke",)),
    ("editor-cef-smoke-shell-uimirror", ("editor/shell/**/context_editor_shell_uimirror_smoke",)),
)

# A smoke "reached its verdict" when its output carries one of these. Deliberately generic: every one
# of these executables narrates its own success, and the point is to distinguish "did work then
# wedged" from "wedged before doing work" — not to re-assert what each smoke asserts.
VERDICT_MARKERS = ("booted", "milestone", "rendered", "ok:", "PASS", "verdict")

# Matched on TOKEN boundaries, never as bare substrings. `"PASS"` as a substring also matches
# `password`, `bypass`, `passed` and `passing` — and most of these smokes run with
# `verbose_logging = true`, so Chromium's own stderr shares this buffer, including its chatter about
# the PASSWORD store, which is the very OSCrypt subsystem #437 lives in. A stall BEFORE the smoke's
# own verdict would then be reported `HUNG_AFTER_VERDICT`, inverting the ONE distinction this tool
# exists to make (the two "lead to opposite investigations").
VERDICT_MARKER_RE = re.compile(
    "|".join(rf"\b{re.escape(m)}" + (r"\b" if m[-1].isalnum() else "") for m in VERDICT_MARKERS),
    re.IGNORECASE)

# macOS: the process securityd spawns to ASK a human about a keychain ACL. Its presence during a hang
# is the #437 signature, and it OUTLIVES the client that triggered it, so it is also litter worth
# reporting.
SECURITY_AGENT_FRAGMENT = "SecurityAgent.bundle/Contents/MacOS/SecurityAgent"

# A CEF executable is a process TREE (renderer / GPU / zygote helpers), and every child INHERITS the
# stdout we hand the launcher. Two consequences, both load-bearing here:
#
#   * capture to a FILE, never a PIPE. Draining a pipe after killing only the launcher blocks until
#     EVERY inheritor closes its write end — and the run this tool kills is by definition one whose
#     browser wedged in `CefShutdown()` and therefore never tore its own helpers down. The measurer
#     would hang on exactly the hang it exists to measure, with no budget and no diagnostic.
#   * kill the process GROUP, not the launcher pid, so no helper outlives the run and poisons the
#     next measurement.
#
# `tools/web_golden_run.py` and `tools/webui_test_run.py` already do both for headless Chrome
# (CE #196 — the teardown race that flaked `render (web, emscripten)`); the same rule binds any
# harness that reaps a browser. Their `_terminate_browser` is not shared code yet — extracting one
# would touch two files outside this change — so this is a deliberate third copy of the pattern.
POSIX_PROCESS_GROUPS = os.name == "posix"


def terminate_tree(proc: subprocess.Popen) -> None:
    """SIGKILL the whole process GROUP, then reap. Best effort — teardown must never raise.

    SIGKILL rather than the graceful SIGTERM-then-SIGKILL ladder the two precedents use: by the time
    this is called the diagnostics are already captured and the subject is a process that has proven
    it will not exit on its own. Falls back to the launcher pid where process groups do not exist
    (Windows), where the file-based capture above is what keeps the drain non-blocking anyway.
    """
    if proc.poll() is not None:
        return
    killed_group = False
    if POSIX_PROCESS_GROUPS:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            killed_group = True
        except (AttributeError, ProcessLookupError, PermissionError, OSError):
            killed_group = False
    if not killed_group:
        proc.kill()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        pass


def find_executable(build_dir: Path, patterns: tuple[str, ...]) -> Path | None:
    """The shortest match wins — a macOS bundle nests the same basename under Contents/MacOS."""
    hits: list[Path] = []
    for pattern in patterns:
        hits.extend(p for p in build_dir.glob(pattern) if p.is_file())
        # macOS bundle shape: <name>.app/Contents/MacOS/<name>.
        hits.extend(p for p in build_dir.glob(pattern + ".app/Contents/MacOS/*") if p.is_file())
    if not hits:
        return None
    return sorted(hits, key=lambda p: (len(str(p)), str(p)))[0]


def pending_security_agents() -> list[str]:
    """Any macOS keychain-prompt process currently up (empty everywhere else)."""
    if platform.system() != "Darwin":
        return []
    try:
        out = subprocess.run(["ps", "-Ao", "pid,etime,command"], capture_output=True, text=True,
                             timeout=30, check=False).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [line.strip() for line in out.splitlines() if SECURITY_AGENT_FRAGMENT in line]


def sample_stacks(pid: int, seconds: int, out_path: Path) -> bool:
    """Capture blocked stacks with sample(1). Best effort — absent on non-macOS."""
    if shutil.which("sample") is None:
        return False
    try:
        subprocess.run(["sample", str(pid), str(seconds), "-file", str(out_path)],
                       capture_output=True, timeout=seconds + 60, check=False)
    except (OSError, subprocess.SubprocessError):
        return False
    return out_path.is_file()


def classify(output: str) -> str:
    return "HUNG_AFTER_VERDICT" if VERDICT_MARKER_RE.search(output or "") else "HUNG_NO_VERDICT"


def run_once(exe: Path, budget: int, extra_args: list[str], diag_dir: Path | None,
             label: str) -> dict:
    """One run. Returns its verdict record; never raises on the executable's own behaviour."""
    started = time.monotonic()
    diagnostics: dict = {}
    hung = False
    # A FILE, not a pipe, and its own process group — see POSIX_PROCESS_GROUPS above for why both are
    # required to measure a hang without becoming one.
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen([str(exe), *extra_args], stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=POSIX_PROCESS_GROUPS)
        try:
            proc.wait(timeout=budget)
        except subprocess.TimeoutExpired:
            hung = True
            # STILL RUNNING: diagnose BEFORE killing, or the evidence dies with it.
            diagnostics["security_agents"] = pending_security_agents()
            if diag_dir is not None:
                diag_dir.mkdir(parents=True, exist_ok=True)
                stack_path = diag_dir / f"{label}.sample.txt"
                if sample_stacks(proc.pid, 2, stack_path):
                    diagnostics["stacks"] = str(stack_path)
            terminate_tree(proc)
        seconds = round(time.monotonic() - started, 2)
        log.seek(0)
        output = log.read()

    if hung:
        record = {"verdict": classify(output), "seconds": seconds, "diagnostics": diagnostics}
    else:
        record = {"verdict": "PASS" if proc.returncode == 0 else f"FAIL(rc={proc.returncode})",
                  "seconds": seconds}
    tail = [ln for ln in output.splitlines() if ln.strip()]
    record["last_line"] = tail[-1][:160] if tail else ""
    return record


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", required=True,
                    help="a configured build dir, e.g. src/build/dev (CONTEXT_BUILD_GUI_CEF=ON)")
    ap.add_argument("-k", "--runs", type=int, default=5,
                    help="runs per executable; a rate needs k>=5 (default: 5)")
    ap.add_argument("--budget", type=int, default=60,
                    help="seconds before a run counts as HUNG (default: 60)")
    ap.add_argument("--only", action="append", default=[],
                    help="restrict to this ctest name (repeatable)")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra argument for each run, e.g. --extra-arg=--use-mock-keychain "
                         "(repeatable). NOTE: the Shell's CefInitialize passes CefMainArgs(0, "
                         "nullptr) on POSIX, so a Chromium switch given this way reaches the two "
                         "standalone smokes but NOT the Shell ones")
    ap.add_argument("--diagnostics-dir", default=None,
                    help="where to write sample(1) stacks captured from a hung run")
    ap.add_argument("--json", dest="json_out", default=None, help="write the full record here")
    args = ap.parse_args(argv)

    if args.runs < 1:
        print("[cef-rate] ERROR: -k must be >= 1", file=sys.stderr)
        return 2
    build_dir = Path(args.build_dir).resolve()
    if not build_dir.is_dir():
        print(f"[cef-rate] ERROR: --build-dir {build_dir} is not a directory", file=sys.stderr)
        return 2
    diag_dir = Path(args.diagnostics_dir).resolve() if args.diagnostics_dir else None

    selected = [s for s in SMOKES if not args.only or s[0] in args.only]
    if not selected:
        print(f"[cef-rate] ERROR: --only matched no known smoke; known: "
              f"{', '.join(name for name, _ in SMOKES)}", file=sys.stderr)
        return 2

    results: dict[str, dict] = {}
    for name, patterns in selected:
        exe = find_executable(build_dir, patterns)
        if exe is None:
            # Not an error: most of these are CEF-ON-only, and a CEF-OFF build has none of them.
            print(f"{name:38s} NOT BUILT (skipped)", flush=True)
            results[name] = {"executable": None, "runs": []}
            continue
        runs = []
        for i in range(args.runs):
            record = run_once(exe, args.budget, args.extra_arg, diag_dir, f"{name}-{i}")
            runs.append(record)
            note = ""
            agents = record.get("diagnostics", {}).get("security_agents", [])
            if agents:
                note = f"  [{len(agents)} SecurityAgent prompt(s) pending — see issue #437]"
            print(f"{name:38s} run{i}: {record['verdict']:18s} {record['seconds']:6.2f}s{note}",
                  flush=True)
        results[name] = {"executable": str(exe), "runs": runs}

    print("\n===== RATE SUMMARY =====", flush=True)
    failures = 0
    measured = 0
    for name, entry in results.items():
        if not entry["runs"]:
            print(f"{name:38s} not built")
            continue
        measured += 1
        counts = Counter(r["verdict"] for r in entry["runs"])
        total = len(entry["runs"])
        failures += total - counts.get("PASS", 0)
        print(f"{name:38s} " + "  ".join(f"{v}={c}/{total}" for v, c in counts.most_common()))

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(results, indent=1), encoding="utf-8")
        print(f"\nwrote {args.json_out}")

    if measured == 0:
        print("[cef-rate] ERROR: no CEF smoke executable was found under "
              f"{build_dir} — configure with -DCONTEXT_BUILD_GUI_CEF=ON and build them first",
              file=sys.stderr)
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
