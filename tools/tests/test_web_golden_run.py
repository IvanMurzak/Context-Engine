"""Tests for tools/web_golden_run.py — the in-browser golden-run driver (R-QA-013 coverage).

No browser needed: the collector (the driver's HTTP half — serve the harness dir, receive the
POSTed frames + /done) is exercised with real HTTP requests from urllib against an ephemeral-port
server, exactly the wire the wasm harness uses. Covers the request-path parser, the RGBA->PPM
packer, frame/done collection, malformed-POST rejection, and the driver's failure verdicts
(missing html / no browser / no done signal via a stub browser process).
"""

from __future__ import annotations

import urllib.error
import urllib.request
from pathlib import Path

import pytest
from conftest import load_tool

web_golden_run = load_tool("web_golden_run")
golden_compare = load_tool("golden_compare")


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("path,expected", [
    ("/golden/triangle3d?w=256&h=256", ("triangle3d", 256, 256)),
    ("/golden/sprite2d?w=16&h=8", ("sprite2d", 16, 8)),
    ("/golden/x?w=0&h=8", None),          # non-positive dim
    ("/golden/x?w=a&h=8", None),          # non-numeric
    ("/golden/x", None),                  # no dims
    ("/golden/?w=1&h=1", None),           # no scene
    ("/other/x?w=1&h=1", None),           # wrong prefix
    ("/golden/a/b?w=1&h=1", None),        # extra path segment
])
def test_parse_golden_request(path, expected):
    assert web_golden_run.parse_golden_request(path) == expected


def test_rgba_to_ppm_bytes_round_trip(tmp_path):
    rgba = bytes.fromhex("ff000080" "00ff0080")  # 2x1: red, green (alpha dropped)
    ppm = web_golden_run.rgba_to_ppm_bytes(rgba, 2, 1)
    p = tmp_path / "t.ppm"
    p.write_bytes(ppm)
    w, h, rgb = golden_compare.read_ppm(p)  # the consumer's own reader accepts it
    assert (w, h) == (2, 1)
    assert rgb == bytes.fromhex("ff0000" "00ff00")


def test_rgba_to_ppm_bytes_rejects_bad_size():
    with pytest.raises(ValueError):
        web_golden_run.rgba_to_ppm_bytes(bytes(5), 2, 1)


# ---------------------------------------------------------------------------
# The collector over real HTTP
# ---------------------------------------------------------------------------


def post(url: str, body: bytes) -> int:
    req = urllib.request.Request(url, data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status
    except urllib.error.HTTPError as exc:
        return exc.code


def test_collector_receives_frames_and_done(tmp_path):
    (tmp_path / "page.html").write_text("<html></html>", encoding="utf-8")
    with web_golden_run.GoldenCollector(tmp_path) as collector:
        base = f"http://127.0.0.1:{collector.port}"

        # GET serves the harness directory (how the browser loads the page).
        with urllib.request.urlopen(f"{base}/page.html", timeout=10) as resp:
            assert resp.status == 200

        # Frame POST: accepted and stored under its scene id.
        rgba = bytes(range(16)) * (2 * 2 * 4 // 16)
        assert post(f"{base}/golden/triangle3d?w=2&h=2", rgba) == 200
        assert collector.frames["triangle3d"] == (2, 2, rgba)

        # Wrong payload size and malformed path are rejected, not stored.
        assert post(f"{base}/golden/short?w=2&h=2", bytes(3)) == 400
        assert post(f"{base}/golden/bad", bytes(4)) == 400
        assert post(f"{base}/elsewhere", b"") == 404
        assert "short" not in collector.frames and "bad" not in collector.frames

        # /done carries the harness exit code and releases the wait.
        assert not collector.done.is_set()
        assert post(f"{base}/done?exit=0", b"") == 200
        assert collector.done.is_set()
        assert collector.harness_exit == 0


def test_collector_done_with_bad_exit_defaults_to_failure(tmp_path):
    with web_golden_run.GoldenCollector(tmp_path) as collector:
        base = f"http://127.0.0.1:{collector.port}"
        assert post(f"{base}/done?exit=nan", b"") == 200
        assert collector.harness_exit == 1


# ---------------------------------------------------------------------------
# Driver verdicts (stubbed browser)
# ---------------------------------------------------------------------------


def test_main_missing_html_is_config_error(tmp_path):
    rc = web_golden_run.main(["--html", str(tmp_path / "nope.html"),
                              "--out-dir", str(tmp_path / "out")])
    assert rc == 2


def test_main_no_browser_is_config_error(tmp_path, monkeypatch):
    html = tmp_path / "page.html"
    html.write_text("<html></html>", encoding="utf-8")
    monkeypatch.delenv("CONTEXT_WEB_GOLDEN_BROWSER", raising=False)
    monkeypatch.setattr(web_golden_run.shutil, "which", lambda name: None)
    rc = web_golden_run.main(["--html", str(html), "--out-dir", str(tmp_path / "out")])
    assert rc == 2


def test_main_stub_browser_timeout_fails(tmp_path):
    """A 'browser' that never POSTs /done -> run failure (exit 1) with the log tail dumped."""
    html = tmp_path / "page.html"
    html.write_text("<html></html>", encoding="utf-8")
    import sys
    stub = sys.executable  # `python -c pass`-ish: exits immediately, posts nothing
    rc = web_golden_run.main(["--html", str(html), "--out-dir", str(tmp_path / "out"),
                              "--browser", stub, "--timeout", "2"])
    assert rc == 1
