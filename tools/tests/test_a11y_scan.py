"""Tests for tools/a11y_scan.py — the M5-F6 editor-UI accessibility gate (R-QA-013 coverage).

Covers the axe-class DOM scan (clean panel, missing accessible name, duplicate id, unreachable
command), the JSONL coverage-manifest loader (comments/blanks, malformed line, duplicate id, empty),
the gate verdict (pass / per-panel a11y failure / coverage gap / undeclared panel / keyboard-path
gap), and the CLI exit codes (0 / 1 / 2). The LIVE committed coverage.manifest.jsonl is validated at
the end: it parses and declares the F0b placeholder panel.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import load_tool

a11y_scan = load_tool("a11y_scan")

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_MANIFEST = REPO_ROOT / "src" / "editor" / "gui" / "a11y" / "coverage.manifest.jsonl"

# The rendered semantic HTML of the conformant F0b placeholder panel (uitree::render_html output).
PLACEHOLDER_HTML = (
    '<section id="placeholder.panel" role="region" aria-label="Context Editor">'
    '<h2 id="placeholder.heading" role="heading" aria-label="Context Editor">Context Editor</h2>'
    '<output id="placeholder.status" role="status" aria-label="Status">Editor host online</output>'
    '<button id="placeholder.refresh" role="button" aria-label="Refresh" tabindex="0" '
    'data-command="placeholder.refresh">Refresh</button>'
    "</section>"
)


def placeholder_panel_report(**overrides) -> dict:
    panel = {
        "id": "placeholder",
        "title": "Context Editor",
        "passed": True,
        "violations": [],
        "focus_order": ["placeholder.refresh"],
        "commands": ["placeholder.refresh"],
        "html": PLACEHOLDER_HTML,
    }
    panel.update(overrides)
    return panel


# ---------------------------------------------------------------------------
# axe-class DOM scan
# ---------------------------------------------------------------------------


def test_scan_html_clean_placeholder():
    assert a11y_scan.scan_html(PLACEHOLDER_HTML) == []


def test_scan_html_missing_accessible_name():
    html = '<section id="p" role="region" aria-label="P"><button id="b" role="button">X</button></section>'
    codes = [f["code"] for f in a11y_scan.scan_html(html)]
    assert "missing-name" in codes


def test_scan_html_duplicate_id():
    html = '<section id="dup" role="region" aria-label="P"><span id="dup" role="text">x</span></section>'
    codes = [f["code"] for f in a11y_scan.scan_html(html)]
    assert "duplicate-id" in codes


def test_scan_html_unreachable_command():
    # a command carrier with no tabindex has no keyboard path
    html = ('<section id="p" role="region" aria-label="P">'
            '<button id="b" role="button" aria-label="B" data-command="do.thing">B</button></section>')
    codes = [f["code"] for f in a11y_scan.scan_html(html)]
    assert "unreachable-command" in codes


def test_scan_html_empty_is_clean():
    assert a11y_scan.scan_html("") == []


# ---------------------------------------------------------------------------
# manifest loader
# ---------------------------------------------------------------------------


def test_load_manifest_happy(tmp_path):
    p = tmp_path / "m.jsonl"
    p.write_text(
        "# a comment\n"
        "\n"
        '{"id": "placeholder", "title": "Context Editor", "owner": "M5-F0b"}\n'
        '{"id": "scenetree", "title": "Scene", "owner": "M5-F2"}\n',
        encoding="utf-8",
    )
    m = a11y_scan.load_manifest(p)
    assert set(m.keys()) == {"placeholder", "scenetree"}
    assert m["scenetree"]["owner"] == "M5-F2"


def test_load_manifest_malformed_line(tmp_path):
    p = tmp_path / "m.jsonl"
    p.write_text('{"id": "ok"}\nnot json\n', encoding="utf-8")
    with pytest.raises(ValueError):
        a11y_scan.load_manifest(p)


def test_load_manifest_duplicate_id(tmp_path):
    p = tmp_path / "m.jsonl"
    p.write_text('{"id": "x"}\n{"id": "x"}\n', encoding="utf-8")
    with pytest.raises(ValueError):
        a11y_scan.load_manifest(p)


def test_load_manifest_empty(tmp_path):
    p = tmp_path / "m.jsonl"
    p.write_text("# only comments\n\n", encoding="utf-8")
    with pytest.raises(ValueError):
        a11y_scan.load_manifest(p)


# ---------------------------------------------------------------------------
# gate verdict
# ---------------------------------------------------------------------------


def test_gate_pass():
    report = {"panels": [placeholder_panel_report()]}
    manifest = {"placeholder": {"id": "placeholder"}}
    verdict = a11y_scan.gate(report, manifest)
    assert verdict["pass"] is True
    assert verdict["panels"]["placeholder"]["pass"] is True


def test_gate_coverage_gap_declared_not_scanned():
    report = {"panels": [placeholder_panel_report()]}
    manifest = {"placeholder": {"id": "placeholder"}, "scenetree": {"id": "scenetree"}}
    verdict = a11y_scan.gate(report, manifest)
    assert verdict["pass"] is False
    assert verdict["coverage"]["missing"] == ["scenetree"]


def test_gate_undeclared_panel_scanned():
    report = {"panels": [placeholder_panel_report(), placeholder_panel_report(id="ghost")]}
    manifest = {"placeholder": {"id": "placeholder"}}
    verdict = a11y_scan.gate(report, manifest)
    assert verdict["pass"] is False
    assert verdict["coverage"]["undeclared"] == ["ghost"]


def test_gate_harness_violation_fails():
    bad = placeholder_panel_report(
        passed=False,
        violations=[{"node_id": "b", "code": "missing-name", "message": "no name"}],
    )
    verdict = a11y_scan.gate({"panels": [bad]}, {"placeholder": {"id": "placeholder"}})
    assert verdict["pass"] is False
    codes = [f["code"] for f in verdict["panels"]["placeholder"]["findings"]]
    assert "missing-name" in codes


def test_gate_dom_scan_catches_html_regression():
    # harness claims passed=True, but the rendered HTML has an unnamed button — the independent DOM
    # scan must still fail the panel.
    regressed = placeholder_panel_report(
        html='<section id="p" role="region" aria-label="P"><button id="b" role="button">X</button></section>'
    )
    verdict = a11y_scan.gate({"panels": [regressed]}, {"placeholder": {"id": "placeholder"}})
    assert verdict["pass"] is False
    codes = [f["code"] for f in verdict["panels"]["placeholder"]["findings"]]
    assert "missing-name" in codes


def test_gate_keyboard_path_gap():
    no_focus = placeholder_panel_report(focus_order=[], html="<section id='p' role='group'></section>")
    verdict = a11y_scan.gate({"panels": [no_focus]}, {"placeholder": {"id": "placeholder"}})
    assert verdict["pass"] is False
    codes = [f["code"] for f in verdict["panels"]["placeholder"]["findings"]]
    assert "no-keyboard-path" in codes


def test_gate_missing_panels_array_raises():
    with pytest.raises(ValueError):
        a11y_scan.gate({}, {"placeholder": {"id": "placeholder"}})


def test_gate_panel_without_id_raises():
    # a report panel missing its 'id' is a malformed report -> clean config error (ValueError ->
    # main() exit 2), not an uncaught TypeError from sorting a None panel id.
    report = {"panels": [{"title": "no id here", "passed": True}]}
    with pytest.raises(ValueError):
        a11y_scan.gate(report, {"placeholder": {"id": "placeholder"}})


# ---------------------------------------------------------------------------
# CLI exit codes
# ---------------------------------------------------------------------------


def _write(tmp_path: Path, report: dict, manifest_lines: str) -> tuple[Path, Path]:
    rp = tmp_path / "report.json"
    mp = tmp_path / "coverage.manifest.jsonl"
    rp.write_text(json.dumps(report), encoding="utf-8")
    mp.write_text(manifest_lines, encoding="utf-8")
    return rp, mp


def test_main_pass(tmp_path):
    rp, mp = _write(tmp_path, {"panels": [placeholder_panel_report()]},
                    '{"id": "placeholder"}\n')
    assert a11y_scan.main(["--report", str(rp), "--manifest", str(mp)]) == 0


def test_main_fail(tmp_path):
    rp, mp = _write(tmp_path, {"panels": [placeholder_panel_report()]},
                    '{"id": "placeholder"}\n{"id": "scenetree"}\n')
    assert a11y_scan.main(["--report", str(rp), "--manifest", str(mp)]) == 1


def test_main_config_error_missing_report(tmp_path):
    mp = tmp_path / "coverage.manifest.jsonl"
    mp.write_text('{"id": "placeholder"}\n', encoding="utf-8")
    assert a11y_scan.main(["--report", str(tmp_path / "nope.json"), "--manifest", str(mp)]) == 2


def test_main_config_error_bad_manifest(tmp_path):
    rp = tmp_path / "report.json"
    rp.write_text(json.dumps({"panels": [placeholder_panel_report()]}), encoding="utf-8")
    mp = tmp_path / "coverage.manifest.jsonl"
    mp.write_text("not json\n", encoding="utf-8")
    assert a11y_scan.main(["--report", str(rp), "--manifest", str(mp)]) == 2


# ---------------------------------------------------------------------------
# live committed manifest
# ---------------------------------------------------------------------------


def test_live_manifest_parses_and_declares_placeholder():
    manifest = a11y_scan.load_manifest(LIVE_MANIFEST)
    assert "placeholder" in manifest
    assert manifest["placeholder"]["title"] == "Context Editor"
