#!/usr/bin/env python3
"""Editor-UI accessibility gate (M5-F6, issue #154 — R-A11Y-001 / R-EDIT-001).

Consumes the JSON report emitted by the headless `context_gui_a11y_scan` harness (which audits every
registered editor panel's UI-logic tree) and enforces the R-A11Y-001 per-panel gate in CI:

  * axe-core-class semantic/ARIA scan over each panel's RENDERED DOM string — re-audited here,
    independently of the C++ harness, straight off the emitted HTML (no browser / no CEF): every
    name-requiring / interactive role carries a non-empty accessible name, ids are unique, and every
    command carrier is keyboard-focusable;
  * keyboard-only navigation — every exposed command has a focus path;
  * the harness's own violation list is empty (belt-and-braces);
  * COVERAGE cross-check — the set of scanned panels exactly matches coverage.manifest.jsonl (a panel
    declared but not scanned, or scanned but not declared, is a coverage failure).

Modelled on tools/golden_compare.py — pure stdlib, compare-only. Exit 0 = all panels pass +
coverage matches; 1 = an a11y violation or a coverage gap; 2 = a configuration/IO error.
"""

from __future__ import annotations

import argparse
import json
import sys
from html.parser import HTMLParser
from pathlib import Path

# ARIA roles that REQUIRE an accessible name (aria-label) to be conformant — mirrors
# uitree::role_requires_name (node.cpp) plus the interactive roles below.
NAME_REQUIRING_ROLES = {
    "region",
    "treeitem",
    "listitem",
    "button",
    "textbox",
    "checkbox",
    "heading",
    "status",
}

# Interactive roles: a control the user operates. Always name-requiring, and (when it carries a
# command) it must be reachable via the keyboard.
INTERACTIVE_ROLES = {"button", "textbox", "checkbox", "link", "menuitem"}


class _DomA11yScanner(HTMLParser):
    """An axe-core-class semantic/ARIA scan over a rendered panel's HTML string.

    Re-derives the R-A11Y-001 findings from the DOM the CEF host would paint, independently of the
    C++ audit — so a semantic-HTML / ARIA regression in render_html() is caught here even if the
    tree-level audit were to miss it.
    """

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.findings: list[dict] = []
        self._ids: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        a = {name: (value or "") for name, value in attrs}
        node_id = a.get("id", "")
        role = a.get("role", "")
        label = a.get("aria-label", "")
        has_tabindex = "tabindex" in a
        command = a.get("data-command", "")

        if node_id:
            if node_id in self._ids:
                self._finding(node_id, "duplicate-id",
                              f'node id "{node_id}" is not unique within the panel')
            self._ids.add(node_id)

        if role in NAME_REQUIRING_ROLES or role in INTERACTIVE_ROLES:
            if not label:
                self._finding(node_id, "missing-name",
                              f'node "{node_id}" (role {role or "?"}) has no accessible name '
                              f"(aria-label)")

        # A node that carries a command must be keyboard-focusable, or the action has no keyboard
        # path — the DOM analog of the harness's unreachable-command finding.
        if command and not has_tabindex:
            self._finding(node_id, "unreachable-command",
                          f'node "{node_id}" binds command "{command}" but is not keyboard-focusable '
                          f"(no tabindex)")

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        # Self-closing form (e.g. <input .../>) — audit it the same as an open tag.
        self.handle_starttag(tag, attrs)

    def _finding(self, node_id: str, code: str, message: str) -> None:
        self.findings.append({"node_id": node_id, "code": code, "message": message})


def scan_html(html: str) -> list[dict]:
    """Axe-class semantic/ARIA findings over a rendered panel HTML string (possibly empty)."""
    scanner = _DomA11yScanner()
    scanner.feed(html)
    scanner.close()
    return scanner.findings


def load_manifest(path: Path) -> dict[str, dict]:
    """Load coverage.manifest.jsonl → {panel_id: entry}. Skips blank / '#'-comment lines.

    Raises ValueError on a malformed entry (bad JSON, missing/duplicate id).
    """
    entries: dict[str, dict] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            entry = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or not entry["id"]:
            raise ValueError(f"{path}:{lineno}: entry needs a non-empty string 'id'")
        pid = entry["id"]
        if pid in entries:
            raise ValueError(f"{path}:{lineno}: duplicate panel id {pid!r}")
        entries[pid] = entry
    if not entries:
        raise ValueError(f"{path}: no panel entries")
    return entries


def gate(report: dict, manifest: dict[str, dict]) -> dict:
    """Enforce the R-A11Y-001 gate over the harness report + coverage manifest. Returns a verdict."""
    panels = report.get("panels")
    if not isinstance(panels, list):
        raise ValueError("report has no 'panels' array")
    report_panels: dict[str, dict] = {}
    for p in panels:
        if not isinstance(p, dict):
            continue
        pid = p.get("id")
        if not isinstance(pid, str) or not pid:
            raise ValueError(f"report panel missing a non-empty string 'id': {p!r}")
        report_panels[pid] = p

    declared = set(manifest.keys())
    scanned = set(report_panels.keys())
    missing = sorted(declared - scanned)      # declared but never scanned -> coverage gap
    undeclared = sorted(scanned - declared)   # scanned but not declared   -> undeclared panel

    panel_results: dict[str, dict] = {}
    all_pass = not missing and not undeclared
    for pid in sorted(scanned):
        p = report_panels[pid]
        findings: list[dict] = []
        # (1) independent axe-class DOM re-audit of the rendered HTML.
        findings.extend(scan_html(p.get("html", "")))
        # (2) belt-and-braces: the harness's own tree-level audit must be clean.
        if not p.get("passed", False):
            for v in p.get("violations", []):
                if isinstance(v, dict):
                    findings.append(v)
        # (3) keyboard-only navigation: a panel that exposes commands must have a focus order.
        if p.get("commands") and not p.get("focus_order"):
            findings.append({"node_id": pid, "code": "no-keyboard-path",
                             "message": "panel exposes commands but has an empty focus order"})
        panel_pass = not findings
        all_pass = all_pass and panel_pass
        panel_results[pid] = {"pass": panel_pass, "findings": findings}

    return {
        "pass": all_pass,
        "coverage": {
            "declared": sorted(declared),
            "scanned": sorted(scanned),
            "missing": missing,
            "undeclared": undeclared,
        },
        "panels": panel_results,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--report", required=True,
                    help="JSON report emitted by context_gui_a11y_scan --out")
    ap.add_argument("--manifest", default="src/editor/gui/a11y/coverage.manifest.jsonl",
                    help="coverage manifest (JSON Lines; declared panels)")
    args = ap.parse_args(argv)

    report_path = Path(args.report)
    manifest_path = Path(args.manifest)
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[a11y-scan] ERROR: cannot load report {report_path}: {exc}", file=sys.stderr)
        return 2
    try:
        manifest = load_manifest(manifest_path)
    except (OSError, ValueError) as exc:
        print(f"[a11y-scan] ERROR: cannot load manifest {manifest_path}: {exc}", file=sys.stderr)
        return 2

    try:
        verdict = gate(report, manifest)
    except ValueError as exc:
        print(f"[a11y-scan] ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"[a11y-scan] {json.dumps(verdict, sort_keys=True)}")
    if verdict["pass"]:
        n = len(verdict["panels"])
        print(f"[a11y-scan] PASS: {n} panel(s) conformant; coverage matches the manifest")
        return 0

    cov = verdict["coverage"]
    if cov["missing"]:
        print(f"[a11y-scan] FAIL: panels declared in the manifest but not scanned: {cov['missing']} "
              f"(register them in src/editor/gui/a11y/registry.cpp)", file=sys.stderr)
    if cov["undeclared"]:
        print(f"[a11y-scan] FAIL: panels scanned but not declared in the manifest: "
              f"{cov['undeclared']} (add them to coverage.manifest.jsonl)", file=sys.stderr)
    for pid, res in sorted(verdict["panels"].items()):
        if not res["pass"]:
            for f in res["findings"]:
                print(f"[a11y-scan] FAIL: panel {pid!r} [{f.get('code')}] "
                      f"{f.get('node_id')}: {f.get('message')}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
