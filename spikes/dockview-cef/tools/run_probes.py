#!/usr/bin/env python3
"""Headless-Chromium probe driver for the M9 s1 Dockview-in-CEF spike (THROWAWAY).

Serves web/ over http://127.0.0.1 and opens web/index.html in HEADLESS CHROMIUM (Chrome or the
Chromium-based Edge), which runs the 6-probe matrix (web/probes.js) under the harness's strict
no-inline-SCRIPT CSP and POSTs its JSON verdict back:

    POST /done?exit=<code>   (body: the full probe-result JSON)

This is the spike's runnable self-check (the ctest COMMAND) AND the tool that MEASURES probes 1-4
and 6 in a real Chromium engine. Probe 5 (OS renderer-process isolation) and the custom
`context-editor://` scheme are the CEF-host residuals — measured by src/ on a local MSVC CEF-149
build (see ../FINDINGS.md). Modelled on tools/web_golden_run.py (the repo's established
headless-Chromium POST-back harness).

Exit 0 = every required probe (1,2,3,4,6) PASSed (or --allow-skip and no browser found);
1 = a required probe FAILed or the harness never reported; 2 = configuration error.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HERE = Path(__file__).resolve().parent
WEB_DIR = HERE.parent / "web"

CHROMIUM_FLAGS = [
    "--headless=new",
    "--no-sandbox",
    "--disable-gpu",
    "--disable-dev-shm-usage",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-extensions",
    "--disable-background-networking",
    # IsolateSandboxedIframes is the Chromium default under study (probe 5); leave feature flags
    # at their defaults so the measurement reflects the shipped behavior.
]

# Windows-first discovery (this executor host), then the POSIX names web_golden_run.py uses.
WINDOWS_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]
PATH_CANDIDATES = ["google-chrome", "google-chrome-stable", "chromium-browser", "chromium",
                   "chrome", "msedge", "microsoft-edge"]


def find_browser(explicit: str | None) -> str | None:
    for c in ([explicit] if explicit else []) + [os.environ.get("CONTEXT_SPIKE_BROWSER")]:
        if c and (shutil.which(c) or Path(c).exists()):
            return c
    for p in WINDOWS_CANDIDATES:
        if Path(p).exists():
            return p
    for name in PATH_CANDIDATES:
        found = shutil.which(name)
        if found:
            return found
    return None


class ProbeCollector:
    def __init__(self, serve_dir: Path):
        self.verdict: dict | None = None
        self.exit_code: int | None = None
        self.done = threading.Event()
        collector = self

        class Handler(SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=str(serve_dir), **k)

            def do_POST(self):  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length > 0 else b""
                if urlparse(self.path).path == "/done":
                    q = parse_qs(urlparse(self.path).query)
                    try:
                        collector.exit_code = int(q.get("exit", ["1"])[0])
                    except ValueError:
                        collector.exit_code = 1
                    try:
                        collector.verdict = json.loads(body.decode("utf-8"))
                    except Exception:
                        collector.verdict = {"parse_error": body[:400].decode("utf-8", "replace")}
                    self.send_response(200); self.end_headers()
                    collector.done.set()
                    return
                self.send_response(404); self.end_headers()

            def log_message(self, *a):  # quiet
                return

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self._server.server_address[1]

    def __enter__(self):
        self._thread.start(); return self

    def __exit__(self, *exc):
        self._server.shutdown(); self._server.server_close()


def _terminate(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        else:
            proc.terminate()
    except (AttributeError, ProcessLookupError, PermissionError, OSError):
        proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        try:
            proc.kill()
        except OSError:
            pass


def print_matrix(verdict: dict) -> None:
    print("\n=== Dockview-in-CEF probe matrix (measured) ===")
    print(f"    engine: {verdict.get('meta', {}).get('ua', '?')}")
    print(f"    origin: {verdict.get('meta', {}).get('origin', '?')}  "
          f"protocol: {verdict.get('meta', {}).get('protocol', '?')}")
    probes = verdict.get("probes", {})
    for key in sorted(probes, key=lambda k: probes[k].get("n", 0)):
        p = probes[key]
        verdict_str = {True: "PASS", False: "FAIL"}.get(p.get("pass"), str(p.get("pass")).upper())
        print(f"    [{verdict_str:>7}] probe {p.get('n')}: {p.get('name')}")
        print(f"              {p.get('notes')}")
    csp = verdict.get("cspViolations", [])
    print(f"    CSP violations observed: {len(csp)}")
    for v in csp:
        print(f"      - {v.get('directive')}: {v.get('blockedURI')} {v.get('sample','')}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--browser", default=None, help="browser binary (default: $CONTEXT_SPIKE_BROWSER, then discovery)")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the /done verdict")
    ap.add_argument("--out", default=None, help="write the verdict JSON here")
    ap.add_argument("--allow-skip", action="store_true",
                    help="exit 0 (SKIP) instead of 2 when no browser is found (ctest self-check default)")
    args = ap.parse_args(argv)

    if not (WEB_DIR / "index.html").is_file():
        print(f"[dockview-spike] ERROR: harness not found: {WEB_DIR/'index.html'}", file=sys.stderr)
        return 2
    browser = find_browser(args.browser)
    if browser is None:
        msg = "[dockview-spike] no Chromium-family browser found (set --browser or $CONTEXT_SPIKE_BROWSER)"
        if args.allow_skip:
            print(msg + " — SKIP (self-check)"); return 0
        print("ERROR: " + msg, file=sys.stderr); return 2

    with ProbeCollector(WEB_DIR) as col, \
            tempfile.TemporaryDirectory(prefix="dockview-spike-", ignore_cleanup_errors=True) as profile:
        url = f"http://127.0.0.1:{col.port}/index.html"
        cmd = [browser, *CHROMIUM_FLAGS, f"--user-data-dir={profile}", url]
        print(f"[dockview-spike] {Path(browser).name} (headless) -> {url}")
        popen_kw = {"start_new_session": True} if os.name == "posix" else {}
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **popen_kw)
        try:
            got = col.done.wait(timeout=args.timeout)
        finally:
            _terminate(proc)

    if not got:
        print(f"[dockview-spike] FAIL: no /done verdict within {args.timeout:.0f}s", file=sys.stderr)
        return 1
    if args.out:
        Path(args.out).write_text(json.dumps(col.verdict, indent=2), encoding="utf-8")
    print_matrix(col.verdict)
    exit_code = col.exit_code if col.exit_code is not None else 1
    print(f"\n[dockview-spike] harness exit = {exit_code}")
    return 0 if exit_code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
