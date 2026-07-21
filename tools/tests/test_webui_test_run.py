"""Tests for tools/webui_test_run.py — the headless-browser driver for the editor-core T1 TS unit
tier (M9 e07a; R-QA-013 coverage).

No browser needed: the pure verdict helpers (parse_summary / verdict) and the collector's HTTP half
(serve the asset dir, receive the POSTed /report) are exercised directly and over real HTTP against
an ephemeral-port server — the exact wire the test bundle uses. Covers the report parser's totality,
the pass/fail verdict, config-error exits (missing asset dir / bundle / page / browser), the
stub-browser timeout, and the web_golden-inherited process-group teardown robustness.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

import pytest
from conftest import load_tool

webui_test_run = load_tool("webui_test_run")


# ---------------------------------------------------------------------------
# parse_summary — totality against anything the browser could POST
# ---------------------------------------------------------------------------


def test_parse_summary_accepts_a_well_formed_verdict():
    body = b'{"passed": 3, "failed": 0, "failures": []}'
    assert webui_test_run.parse_summary(body) == {"passed": 3, "failed": 0, "failures": []}


def test_parse_summary_keeps_failure_entries():
    body = b'{"passed": 1, "failed": 1, "failures": [{"name": "t", "error": "boom"}]}'
    parsed = webui_test_run.parse_summary(body)
    assert parsed is not None
    assert parsed["failures"] == [{"name": "t", "error": "boom"}]


@pytest.mark.parametrize("body", [
    b"not json",
    b"[]",                                           # not an object
    b'"a string"',                                   # not an object
    b'{"passed": 1, "failed": 0}',                   # missing failures
    b'{"passed": "x", "failed": 0, "failures": []}',  # non-int passed
    b'{"passed": 1, "failed": "x", "failures": []}',  # non-int failed
    b'{"passed": true, "failed": 0, "failures": []}',  # bool is not an int count
    b'{"passed": 1, "failed": 0, "failures": {}}',   # failures not a list
    b"",                                             # empty
])
def test_parse_summary_rejects_malformed(body):
    assert webui_test_run.parse_summary(body) is None


# ---------------------------------------------------------------------------
# verdict — the ctest pass/fail decision
# ---------------------------------------------------------------------------


def test_verdict_pass_when_no_failures():
    code, message = webui_test_run.verdict({"passed": 5, "failed": 0, "failures": []})
    assert code == 0
    assert "5" in message


def test_verdict_fail_names_each_failure():
    code, message = webui_test_run.verdict(
        {"passed": 2, "failed": 1, "failures": [{"name": "parsePanelManifest", "error": "id"}]})
    assert code == 1
    assert "parsePanelManifest" in message and "id" in message


def test_verdict_none_report_is_failure():
    code, message = webui_test_run.verdict(None)
    assert code == 1
    assert "no readable verdict" in message


# ---------------------------------------------------------------------------
# find_browser — resolution order
# ---------------------------------------------------------------------------


def test_find_browser_explicit_path_that_exists(tmp_path):
    fake = tmp_path / "chrome"
    fake.write_text("#!/bin/sh\n", encoding="utf-8")
    assert webui_test_run.find_browser(str(fake)) == str(fake)


def test_find_browser_env_var(tmp_path, monkeypatch):
    fake = tmp_path / "edge"
    fake.write_text("#!/bin/sh\n", encoding="utf-8")
    monkeypatch.setenv("CONTEXT_WEBUI_TEST_BROWSER", str(fake))
    assert webui_test_run.find_browser(None) == str(fake)


def test_find_browser_none_when_absent(monkeypatch):
    monkeypatch.delenv("CONTEXT_WEBUI_TEST_BROWSER", raising=False)
    monkeypatch.setattr(webui_test_run.shutil, "which", lambda name: None)
    assert webui_test_run.find_browser(None) is None


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


def test_collector_serves_dir_and_receives_report(tmp_path):
    (tmp_path / "harness.html").write_text("<html></html>", encoding="utf-8")
    with webui_test_run.ReportCollector(tmp_path) as collector:
        base = f"http://127.0.0.1:{collector.port}"

        # GET serves the asset dir (how the browser loads the harness page + bundle).
        with urllib.request.urlopen(f"{base}/harness.html", timeout=10) as resp:
            assert resp.status == 200

        # An unknown POST path is rejected and does not release the wait.
        assert post(f"{base}/elsewhere", b"") == 404
        assert not collector.done.is_set()

        # /report stores the body and releases the wait.
        body = b'{"passed": 1, "failed": 0, "failures": []}'
        assert post(f"{base}/report", body) == 200
        assert collector.done.is_set()
        assert collector.report_body == body


# ---------------------------------------------------------------------------
# Driver verdicts (config errors + stub browser)
# ---------------------------------------------------------------------------


def _asset_dir_with_bundle(tmp_path: Path) -> Path:
    asset = tmp_path / "assets"
    asset.mkdir()
    (asset / "editor-core.test.js").write_text("// bundle\n", encoding="utf-8")
    (asset / "harness.html").write_text("<html></html>", encoding="utf-8")
    return asset


def test_main_missing_asset_dir_is_config_error(tmp_path):
    assert webui_test_run.main(["--asset-dir", str(tmp_path / "nope")]) == 2


def test_main_missing_bundle_is_config_error(tmp_path):
    asset = tmp_path / "assets"
    asset.mkdir()
    (asset / "harness.html").write_text("<html></html>", encoding="utf-8")
    assert webui_test_run.main(["--asset-dir", str(asset)]) == 2


def test_main_missing_page_is_config_error(tmp_path):
    asset = tmp_path / "assets"
    asset.mkdir()
    (asset / "editor-core.test.js").write_text("// bundle\n", encoding="utf-8")
    assert webui_test_run.main(["--asset-dir", str(asset)]) == 2


def test_main_no_browser_is_config_error(tmp_path, monkeypatch):
    asset = _asset_dir_with_bundle(tmp_path)
    monkeypatch.delenv("CONTEXT_WEBUI_TEST_BROWSER", raising=False)
    monkeypatch.setattr(webui_test_run.shutil, "which", lambda name: None)
    assert webui_test_run.main(["--asset-dir", str(asset)]) == 2


def test_main_stub_browser_timeout_fails(tmp_path):
    """A 'browser' that never POSTs /report -> run failure (exit 1), not a hang."""
    asset = _asset_dir_with_bundle(tmp_path)
    stub = sys.executable  # exits immediately, posts nothing
    rc = webui_test_run.main(["--asset-dir", str(asset), "--browser", stub, "--timeout", "2"])
    assert rc == 1


# ---------------------------------------------------------------------------
# Teardown robustness (inherited from web_golden_run.py's [Errno 39] flake fix)
# ---------------------------------------------------------------------------


def test_terminate_browser_is_safe_on_already_exited_process():
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    webui_test_run._terminate_browser(proc)  # must be a no-op, not an error
    assert proc.poll() is not None


def test_terminate_browser_reaps_live_process():
    proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                            start_new_session=True)
    assert proc.poll() is None
    webui_test_run._terminate_browser(proc)
    assert proc.poll() is not None


def test_temporary_profile_supports_ignore_cleanup_errors():
    with tempfile.TemporaryDirectory(prefix="ctx-webui-test-test-",
                                     ignore_cleanup_errors=True) as profile:
        default = Path(profile) / "Default"
        default.mkdir()
        (default / "leftover").write_bytes(b"x")
    # No exception escaped the with-block: the contract holds.
