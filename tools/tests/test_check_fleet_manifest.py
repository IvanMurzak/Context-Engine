"""Tests for tools/check_fleet_manifest.py — the R-QA-012 fleet-manifest validator (R-QA-013 coverage).

Covers the happy path (the live committed manifest validates against the live workflow), plus each
violation class: unknown runner class, bad red-X policy / tier, the advisory-until-provisioned rule,
a quarantine gate missing its issue, a duplicate id, and a claimed CI job that does not exist in the
workflow. Synthetic manifests are built inline; the final test exercises the real repo artifacts.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

from conftest import load_tool

check_fleet_manifest = load_tool("check_fleet_manifest")

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_MANIFEST = REPO_ROOT / "docs" / "ci-fleet-manifest.json"
LIVE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci.yml"


def base_manifest() -> dict:
    return {
        "manifest_version": 1,
        "runner_classes": {
            "gh-ubuntu-shared": {"os": "ubuntu-latest", "isolation": "shared", "provisioned": True},
            "perf-box": {"os": "linux-x64", "isolation": "perf-isolated", "provisioned": False},
        },
        "gates": [
            {"id": "build", "runner_class": "gh-ubuntu-shared", "tier": "per-PR",
             "red_x_policy": "blocking", "ci_job_id": "build"},
            {"id": "perf", "runner_class": "perf-box", "tier": "nightly",
             "red_x_policy": "advisory", "ci_job_id": None},
        ],
    }


WORKFLOW = "jobs:\n  build:\n    runs-on: ubuntu-latest\n  license-gate:\n    runs-on: ubuntu-latest\n"


def test_valid_manifest_passes():
    assert check_fleet_manifest.validate(base_manifest(), WORKFLOW) == []


def test_unknown_runner_class():
    m = base_manifest()
    m["gates"][0]["runner_class"] = "ghost"
    errors = check_fleet_manifest.validate(m, None)
    assert any("unknown runner_class" in e for e in errors)


def test_bad_red_x_policy():
    m = base_manifest()
    m["gates"][0]["red_x_policy"] = "maybe"
    errors = check_fleet_manifest.validate(m, None)
    assert any("red_x_policy" in e for e in errors)


def test_bad_tier():
    m = base_manifest()
    m["gates"][0]["tier"] = "weekly"
    errors = check_fleet_manifest.validate(m, None)
    assert any("tier" in e for e in errors)


def test_unprovisioned_must_be_advisory():
    m = base_manifest()
    m["gates"][1]["red_x_policy"] = "blocking"  # perf-box is unprovisioned
    errors = check_fleet_manifest.validate(m, None)
    assert any("advisory-until-provisioned" in e for e in errors)


def test_quarantine_needs_issue():
    m = base_manifest()
    m["gates"][0]["red_x_policy"] = "quarantine-with-issue"  # no 'issue' key
    errors = check_fleet_manifest.validate(m, None)
    assert any("requires a non-empty 'issue'" in e for e in errors)


def test_quarantine_with_issue_ok():
    m = base_manifest()
    m["gates"][0]["red_x_policy"] = "quarantine-with-issue"
    m["gates"][0]["issue"] = "Owner/Repo#24"
    assert check_fleet_manifest.validate(m, WORKFLOW) == []


def test_duplicate_gate_id():
    m = base_manifest()
    m["gates"].append(copy.deepcopy(m["gates"][0]))
    errors = check_fleet_manifest.validate(m, None)
    assert any("duplicate id" in e for e in errors)


def test_ci_job_must_exist_in_workflow():
    m = base_manifest()
    m["gates"][0]["ci_job_id"] = "nonexistent-job"
    errors = check_fleet_manifest.validate(m, WORKFLOW)
    assert any("no matching job in the workflow" in e for e in errors)


def test_missing_gates_array():
    errors = check_fleet_manifest.validate({"manifest_version": 1, "runner_classes": {
        "x": {"isolation": "shared", "provisioned": True}}}, None)
    assert any("gates must be a non-empty array" in e for e in errors)


def test_live_manifest_validates_against_live_workflow():
    """The committed manifest must validate against the committed workflow (the real R-QA-012 tie)."""
    manifest = json.loads(LIVE_MANIFEST.read_text(encoding="utf-8"))
    workflow_text = LIVE_WORKFLOW.read_text(encoding="utf-8")
    assert check_fleet_manifest.validate(manifest, workflow_text) == []


def test_live_main_exit_zero():
    rc = check_fleet_manifest.main(["--manifest", str(LIVE_MANIFEST),
                                    "--ci-workflow", str(LIVE_WORKFLOW)])
    assert rc == 0
