#!/usr/bin/env python3
"""Headless-browser driver for the editor-core T1 TS unit tier (M9 e07a — the `webui-ts-unit` ctest).

Serves the built test asset dir (`editor-core.test.js` + `harness.html`), opens the harness page in
HEADLESS CHROMIUM — the same browser family the CI already provisions for the render-web / cef legs,
with NO new npm dependency — and waits for the verdict the bundled test entry POSTs back over its own
origin:

    POST /report   (body: JSON {"passed": int, "failed": int, "failures": [{"name","error"}]})

The bundle (core/src/test/main.ts) runs every pure-TS unit test in-browser and POSTs this one verdict.
Exit code 0 = the harness reported and every assertion passed; 1 = a failed assertion, a malformed
report, or no report within the timeout (a bundle that threw on load); 2 = configuration error
(missing asset dir / bundle / page, or no browser found).

This mirrors tools/web_golden_run.py's hard-won headless-Chromium teardown handling (reap the whole
browser process GROUP, tolerate profile-cleanup races) so the tier does not inherit that job's flakes.
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
from urllib.parse import urlparse

# Minimal, deterministic, GPU-less headless flags — this tier renders no GPU content (unlike
# web_golden_run.py, it needs no WebGPU/SwiftShader flags); it only executes pure-TS assertions.
CHROMIUM_FLAGS = [
    "--headless=new",
    "--no-sandbox",
    "--disable-dev-shm-usage",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-extensions",
    "--disable-background-networking",
    "--disable-gpu",
]

# Chromium family only: it is the CI's reference browser (render-web) and the one editor-cef-smoke
# embeds; `msedge`/`microsoft-edge` are Chromium too (the local Windows repro browser — test.md).
BROWSER_CANDIDATES = [
    "google-chrome",
    "google-chrome-stable",
    "chromium-browser",
    "chromium",
    "chrome",
    "microsoft-edge",
    "microsoft-edge-stable",
    "msedge",
]


def parse_summary(body: bytes) -> dict | None:
    """Parse + structurally validate a /report body. Returns the summary dict or None when malformed.

    Total against anything the browser could POST: a non-object, a missing/!int count, or a
    non-list `failures` all read as None (an unreadable verdict), never a crash.
    """
    try:
        value = json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(value, dict):
        return None
    passed = value.get("passed")
    failed = value.get("failed")
    failures = value.get("failures")
    if not isinstance(passed, int) or isinstance(passed, bool):
        return None
    if not isinstance(failed, int) or isinstance(failed, bool):
        return None
    if not isinstance(failures, list):
        return None
    return {"passed": passed, "failed": failed, "failures": failures}


def verdict(summary: dict | None) -> tuple[int, str]:
    """Decide the ctest exit code + a human message from a parsed summary (or None = no/bad report)."""
    if summary is None:
        return 1, "no readable verdict was reported (the test bundle threw on load or never ran)"
    passed = summary["passed"]
    failed = summary["failed"]
    if failed == 0:
        return 0, f"all {passed} webui TS unit tests passed"
    lines = [f"{failed} of {passed + failed} webui TS unit tests FAILED:"]
    for failure in summary["failures"]:
        if isinstance(failure, dict):
            lines.append(f"  - {failure.get('name', '<unnamed>')}: {failure.get('error', '')}")
        else:
            lines.append(f"  - {failure!r}")
    return 1, "\n".join(lines)


class ReportCollector:
    """Serves the test asset dir and captures the single verdict the harness POSTs to /report."""

    def __init__(self, serve_dir: Path):
        self.serve_dir = serve_dir
        self.report_body: bytes | None = None
        self.done = threading.Event()
        collector = self

        class Handler(SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(serve_dir), **kwargs)

            def do_POST(self):  # noqa: N802 (http.server API name)
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length > 0 else b""
                if urlparse(self.path).path == "/report":
                    # Commit the verdict + release the waiter BEFORE acknowledging the request, so an
                    # observer that sees the 200 is guaranteed to see `report_body`/`done` already set
                    # (main() waits on the Event, not the response — this only tightens the ordering).
                    collector.report_body = body
                    collector.done.set()
                    self.send_response(200)
                    self.end_headers()
                    return
                self.send_response(404)
                self.end_headers()

            def log_message(self, fmt, *args):  # quiet: the driver prints its own progress
                del fmt, args

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self._server.server_address[1]

    def __enter__(self) -> "ReportCollector":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._server.shutdown()
        self._server.server_close()


def find_browser(explicit: str | None) -> str | None:
    """Resolve a Chromium-family browser: --browser, then $CONTEXT_WEBUI_TEST_BROWSER, then PATH."""
    def resolve(candidate: str) -> str | None:
        """A named-or-path candidate kept when it resolves on PATH or exists on disk, else None."""
        return candidate if shutil.which(candidate) or Path(candidate).exists() else None
    if explicit:
        return resolve(explicit)
    env = os.environ.get("CONTEXT_WEBUI_TEST_BROWSER")
    if env:
        return resolve(env)
    for name in BROWSER_CANDIDATES:
        found = shutil.which(name)
        if found:
            return found
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--asset-dir", required=True,
                    help="the built test asset dir (editor-core.test.js + the harness page)")
    ap.add_argument("--page", default="harness.html", help="the harness HTML page inside --asset-dir")
    ap.add_argument("--bundle-name", default="editor-core.test.js",
                    help="the bundled test entry that must exist in --asset-dir")
    ap.add_argument("--browser", default=None,
                    help="browser binary (default: $CONTEXT_WEBUI_TEST_BROWSER, then PATH lookup)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="seconds to wait for the harness /report signal")
    args = ap.parse_args(argv)

    asset_dir = Path(args.asset_dir)
    page = asset_dir / args.page
    bundle = asset_dir / args.bundle_name
    if not asset_dir.is_dir():
        print(f"[webui-test] ERROR: asset dir not found: {asset_dir}", file=sys.stderr)
        return 2
    if not bundle.is_file():
        print(f"[webui-test] ERROR: test bundle not built: {bundle} "
              "(build the context_editor_webui_test target first)", file=sys.stderr)
        return 2
    if not page.is_file():
        print(f"[webui-test] ERROR: harness page not found: {page}", file=sys.stderr)
        return 2
    browser = find_browser(args.browser)
    if browser is None:
        print("[webui-test] ERROR: no Chromium-family browser found "
              "(set --browser or $CONTEXT_WEBUI_TEST_BROWSER)", file=sys.stderr)
        return 2

    # ignore_cleanup_errors: a throwaway CI profile's teardown must never fail an already-decided
    # test result — Chrome under --no-sandbox spawns a child tree that can re-touch the profile a
    # moment after we ask to delete it (web_golden_run.py's [Errno 39] teardown race).
    with ReportCollector(asset_dir) as collector, \
            tempfile.TemporaryDirectory(prefix="ctx-webui-test-",
                                        ignore_cleanup_errors=True) as profile:
        url = f"http://127.0.0.1:{collector.port}/{args.page}"
        cmd = [browser, *CHROMIUM_FLAGS, f"--user-data-dir={profile}", url]
        print(f"[webui-test] launching: {browser} (headless) -> {url}")
        log_path = asset_dir / "webui-test-browser.log"
        with open(log_path, "wb") as log:
            # start_new_session: own process group so we can reap the whole browser tree on teardown.
            proc = subprocess.Popen(cmd, stdout=log, stderr=log, start_new_session=True)
            try:
                got_report = collector.done.wait(timeout=args.timeout)
            finally:
                _terminate_browser(proc)

        if not got_report:
            print(f"[webui-test] FAIL: no /report from the harness within {args.timeout:.0f}s "
                  f"(browser log: {log_path})", file=sys.stderr)
            _dump_log_tail(log_path)
            return 1

        summary = parse_summary(collector.report_body or b"")
        code, message = verdict(summary)
        stream = sys.stdout if code == 0 else sys.stderr
        print(f"[webui-test] {message}", file=stream)
        if code != 0:
            _dump_log_tail(log_path)
        return code


def _terminate_browser(proc: subprocess.Popen) -> None:
    """Terminate the browser AND its child tree, then wait — the web_golden_run.py teardown, verbatim.

    Chrome under --no-sandbox spawns zygote/GPU children; waiting on the launcher pid alone leaves
    them briefly alive to re-touch the --user-data-dir profile after we delete it. The browser is
    started in its own process group (start_new_session=True), so we signal the whole group.
    Best-effort: an already-exited browser, or a platform without process groups, degrades to a plain
    terminate/kill — teardown must never raise.
    """
    if proc.poll() is not None:
        return

    def _signal(hard: bool) -> None:
        try:
            sig = signal.SIGKILL if hard else signal.SIGTERM
            os.killpg(os.getpgid(proc.pid), sig)
        except (AttributeError, ProcessLookupError, PermissionError, OSError):
            proc.kill() if hard else proc.terminate()

    _signal(hard=False)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        _signal(hard=True)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def _dump_log_tail(log_path: Path, lines: int = 60) -> None:
    try:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:]
    except OSError:
        return
    for line in tail:
        print(f"[browser] {line}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
