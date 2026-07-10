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
LIVE_NIGHTLY = REPO_ROOT / ".github" / "workflows" / "bench-nightly.yml"


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
        "minspec_floors": {
            "requirement": "R-QA-007",
            "platforms": {
                "desktop": {"reference_device": "Iris-Xe-class ultrabook",
                            "target_frame_rate_hz": 60, "runner_class": "perf-box"},
                "linux-server": {"reference_device": "4-vCPU x86-64-v2 server",
                                 "target_tick_rate_hz": 60, "runner_class": "perf-box"},
            },
            "not_applicable": {"android": "trailing SHOULD", "ios": "v2"},
        },
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


# ---------------------------------------------------------------------------
# Rule 7 — the R-QA-007 min-spec floor table (M4 T7, issue #141)
# ---------------------------------------------------------------------------


def test_minspec_floors_required():
    m = base_manifest()
    del m["minspec_floors"]
    errors = check_fleet_manifest.validate(m, None)
    assert any("missing minspec_floors" in e for e in errors)


def test_minspec_floor_needs_reference_device():
    m = base_manifest()
    m["minspec_floors"]["platforms"]["desktop"]["reference_device"] = "  "
    errors = check_fleet_manifest.validate(m, None)
    assert any("reference_device" in e for e in errors)


def test_minspec_floor_needs_exactly_one_target():
    m = base_manifest()
    row = m["minspec_floors"]["platforms"]["desktop"]
    row["target_tick_rate_hz"] = 60  # now BOTH targets present
    errors = check_fleet_manifest.validate(m, None)
    assert any("exactly ONE" in e for e in errors)

    del row["target_frame_rate_hz"]
    del row["target_tick_rate_hz"]  # now NO target
    errors = check_fleet_manifest.validate(m, None)
    assert any("exactly ONE" in e for e in errors)


def test_minspec_floor_target_must_be_positive():
    m = base_manifest()
    m["minspec_floors"]["platforms"]["desktop"]["target_frame_rate_hz"] = 0
    errors = check_fleet_manifest.validate(m, None)
    assert any("positive number" in e for e in errors)


def test_minspec_floor_runner_class_must_be_declared():
    m = base_manifest()
    m["minspec_floors"]["platforms"]["desktop"]["runner_class"] = "ghost-box"
    errors = check_fleet_manifest.validate(m, None)
    assert any("minspec_floors platform 'desktop': unknown runner_class" in e for e in errors)


def test_minspec_floor_scope_notes_required():
    m = base_manifest()
    del m["minspec_floors"]["not_applicable"]["ios"]
    errors = check_fleet_manifest.validate(m, None)
    assert any("not_applicable" in e for e in errors)


def test_live_manifest_commits_the_three_v1_floors():
    """R-QA-007 platform scope: the live manifest must carry the three v1 floors (desktop,
    linux-server, web) with named devices — the M4 exit's committed floor table."""
    manifest = json.loads(LIVE_MANIFEST.read_text(encoding="utf-8"))
    platforms = manifest["minspec_floors"]["platforms"]
    assert {"desktop", "linux-server", "web"} <= set(platforms)
    for row in platforms.values():
        assert row["reference_device"].strip()


def test_live_manifest_validates_against_live_workflows():
    """The committed manifest must validate against the committed workflows (the real R-QA-012
    tie): per-PR gates live in ci.yml, the nightly benchmark gates in bench-nightly.yml."""
    manifest = json.loads(LIVE_MANIFEST.read_text(encoding="utf-8"))
    workflow_text = LIVE_WORKFLOW.read_text(encoding="utf-8") + "\n" + \
        LIVE_NIGHTLY.read_text(encoding="utf-8")
    assert check_fleet_manifest.validate(manifest, workflow_text) == []


def test_live_manifest_nightly_jobs_not_in_ci_yml_alone():
    """Sanity of the multi-workflow need: at least one gate's ci_job_id lives ONLY in the nightly
    workflow, so validating against ci.yml alone must flag it (guards against the nightly gates
    silently pointing at per-PR jobs)."""
    manifest = json.loads(LIVE_MANIFEST.read_text(encoding="utf-8"))
    errors = check_fleet_manifest.validate(manifest, LIVE_WORKFLOW.read_text(encoding="utf-8"))
    assert any("bench-100k-nightly" in e for e in errors)


def test_live_main_exit_zero_with_both_workflows():
    rc = check_fleet_manifest.main(["--manifest", str(LIVE_MANIFEST),
                                    "--ci-workflow", str(LIVE_WORKFLOW),
                                    "--ci-workflow", str(LIVE_NIGHTLY)])
    assert rc == 0


def test_main_multiple_workflows_missing_file_is_config_error(tmp_path):
    rc = check_fleet_manifest.main(["--manifest", str(LIVE_MANIFEST),
                                    "--ci-workflow", str(LIVE_WORKFLOW),
                                    "--ci-workflow", str(tmp_path / "nope.yml")])
    assert rc == 2
