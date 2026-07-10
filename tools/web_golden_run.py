#!/usr/bin/env python3
"""In-browser golden-scene run driver (M4 T7, issue #141 — the "one browser blocking" run gate).

Serves the emscripten-built web render harness (context-render-web.html, the T6 web backend), opens
it in HEADLESS CHROMIUM with software WebGPU (SwiftShader — the browser analog of the native leg's
lavapipe), and collects the golden-corpus frames the harness POSTs back over its own origin:

    POST /golden/<scene>?w=<width>&h=<height>   (body: raw RGBA8, tight rows, top-first)
    POST /done?exit=<code>                      (harness completion + its exit code)

Received frames are written as binary PPMs into --out-dir for tools/golden_compare.py (the SSIM
gate vs goldens/). The HTTP body channel is deliberate: pixel payloads never travel as console
lines Chrome could truncate.

Browser pick (v1 ruling, docs/ci-fleet-manifest.json § minspec_floors + the golden gates): CHROMIUM
— the only browser shipping WebGPU on headless Linux today, and the design record's reference
browser throughout (the M0 spike's Tint/Chrome leg, ARCHITECTURE.md §4). Additional browser rows
join as advisory legs when their R-QA-012 runner classes are provisioned.

Exit code 0 = harness PASSed and every expected scene was collected; 1 = run/collection failure;
2 = configuration error (missing html / no browser found).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

DEFAULT_SCENES = ["triangle3d", "sprite2d"]

# Flags for a deterministic, GPU-less CI box: new headless mode, sandbox off (container-safe),
# WebGPU forced on with the SwiftShader software adapter (Dawn's software path — the browser
# analog of lavapipe on the native leg).
CHROMIUM_FLAGS = [
    "--headless=new",
    "--no-sandbox",
    "--disable-dev-shm-usage",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-extensions",
    "--disable-background-networking",
    "--enable-unsafe-webgpu",
    "--enable-features=Vulkan",
    "--use-webgpu-adapter=swiftshader",
]

BROWSER_CANDIDATES = [
    "google-chrome",
    "google-chrome-stable",
    "chromium-browser",
    "chromium",
    "chrome",
]


def parse_golden_request(path: str) -> tuple[str, int, int] | None:
    """Parse a /golden/<scene>?w=&h= request path. Returns (scene, w, h) or None when malformed."""
    parsed = urlparse(path)
    parts = [p for p in parsed.path.split("/") if p]
    if len(parts) != 2 or parts[0] != "golden" or not parts[1]:
        return None
    query = parse_qs(parsed.query)
    try:
        width = int(query.get("w", ["0"])[0])
        height = int(query.get("h", ["0"])[0])
    except ValueError:
        return None
    if width <= 0 or height <= 0:
        return None
    return parts[1], width, height


def rgba_to_ppm_bytes(rgba: bytes, width: int, height: int) -> bytes:
    """Pack tight RGBA8 rows into a binary P6 PPM (alpha dropped) — golden_compare.py's input."""
    if len(rgba) != width * height * 4:
        raise ValueError(f"payload is {len(rgba)} bytes, want {width * height * 4}")
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    rgb = bytearray(rgba)
    del rgb[3::4]  # drop every 4th byte (alpha) in one pass, leaving tight RGB rows
    return header + bytes(rgb)


class GoldenCollector:
    """Serves the harness directory and collects the frames + done signal it POSTs back."""

    def __init__(self, serve_dir: Path):
        self.serve_dir = serve_dir
        self.frames: dict[str, tuple[int, int, bytes]] = {}
        self.harness_exit: int | None = None
        self.done = threading.Event()
        collector = self

        class Handler(SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(serve_dir), **kwargs)

            def do_POST(self):  # noqa: N802 (http.server API name)
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length > 0 else b""
                parsed = urlparse(self.path)
                if parsed.path.startswith("/golden/"):
                    request = parse_golden_request(self.path)
                    if request is None or len(body) != request[1] * request[2] * 4:
                        self.send_response(400)
                        self.end_headers()
                        return
                    scene, width, height = request
                    collector.frames[scene] = (width, height, body)
                    self.send_response(200)
                    self.end_headers()
                    return
                if parsed.path == "/done":
                    query = parse_qs(parsed.query)
                    try:
                        collector.harness_exit = int(query.get("exit", ["1"])[0])
                    except ValueError:
                        collector.harness_exit = 1
                    self.send_response(200)
                    self.end_headers()
                    collector.done.set()
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

    def __enter__(self) -> "GoldenCollector":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._server.shutdown()
        self._server.server_close()


def find_browser(explicit: str | None) -> str | None:
    if explicit:
        return explicit if shutil.which(explicit) or Path(explicit).exists() else None
    env = os.environ.get("CONTEXT_WEB_GOLDEN_BROWSER")
    if env:
        return env if shutil.which(env) or Path(env).exists() else None
    for name in BROWSER_CANDIDATES:
        found = shutil.which(name)
        if found:
            return found
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--html", required=True,
                    help="the built harness page (context-render-web.html)")
    ap.add_argument("--out-dir", required=True, help="where collected frames land as PPMs")
    ap.add_argument("--scenes", default=",".join(DEFAULT_SCENES),
                    help="comma-separated corpus scenes the harness must deliver")
    ap.add_argument("--browser", default=None,
                    help="browser binary (default: $CONTEXT_WEB_GOLDEN_BROWSER, then PATH lookup)")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="seconds to wait for the harness /done signal")
    args = ap.parse_args(argv)

    html = Path(args.html)
    if not html.is_file():
        print(f"[web-golden] ERROR: harness page not found: {html}", file=sys.stderr)
        return 2
    browser = find_browser(args.browser)
    if browser is None:
        print("[web-golden] ERROR: no Chromium-family browser found "
              "(set --browser or $CONTEXT_WEB_GOLDEN_BROWSER)", file=sys.stderr)
        return 2
    scenes = [s for s in args.scenes.split(",") if s]
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    with GoldenCollector(html.parent) as collector, \
            tempfile.TemporaryDirectory(prefix="ctx-web-golden-") as profile:
        url = f"http://127.0.0.1:{collector.port}/{html.name}"
        cmd = [browser, *CHROMIUM_FLAGS, f"--user-data-dir={profile}", url]
        print(f"[web-golden] launching: {browser} (headless, SwiftShader WebGPU) -> {url}")
        log_path = out_dir / "browser.log"
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(cmd, stdout=log, stderr=log)
            try:
                got_done = collector.done.wait(timeout=args.timeout)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()

        if not got_done:
            print(f"[web-golden] FAIL: no /done from the harness within {args.timeout:.0f}s "
                  f"(browser log: {log_path})", file=sys.stderr)
            _dump_log_tail(log_path)
            return 1

        failures = 0
        for scene in scenes:
            frame = collector.frames.get(scene)
            if frame is None:
                print(f"[web-golden] FAIL: scene {scene!r} was never posted", file=sys.stderr)
                failures += 1
                continue
            width, height, rgba = frame
            ppm = out_dir / f"{scene}.ppm"
            ppm.write_bytes(rgba_to_ppm_bytes(rgba, width, height))
            print(f"[web-golden] collected scene={scene} ({width}x{height}) -> {ppm}")

        verdict = {
            "harness_exit": collector.harness_exit,
            "scenes_collected": sorted(collector.frames),
            "scenes_expected": scenes,
        }
        print(f"[web-golden] {json.dumps(verdict, sort_keys=True)}")
        if collector.harness_exit != 0:
            print(f"[web-golden] FAIL: harness exit {collector.harness_exit} "
                  f"(77 = browser WebGPU unavailable; browser log: {log_path})", file=sys.stderr)
            _dump_log_tail(log_path)
            return 1
        if failures:
            _dump_log_tail(log_path)
            return 1
        return 0


def _dump_log_tail(log_path: Path, lines: int = 60) -> None:
    try:
        tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:]
    except OSError:
        return
    for line in tail:
        print(f"[browser] {line}", file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
