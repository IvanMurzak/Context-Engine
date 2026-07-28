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
    rc = check_fleet_manifest.main(["--repo-root", str(REPO_ROOT),
                                    "--manifest", str(LIVE_MANIFEST),
                                    "--ci-workflow", str(LIVE_WORKFLOW),
                                    "--ci-workflow", str(LIVE_NIGHTLY)])
    assert rc == 0


def test_main_multiple_workflows_missing_file_is_config_error(tmp_path):
    rc = check_fleet_manifest.main(["--repo-root", str(REPO_ROOT),
                                    "--manifest", str(LIVE_MANIFEST),
                                    "--ci-workflow", str(LIVE_WORKFLOW),
                                    "--ci-workflow", str(tmp_path / "nope.yml")])
    assert rc == 2


# --- the --repo-root friction, and the second workflow file the gate now resolves itself (M9 x11) --
# MEASURED before the fix: `--repo-root .` was an argparse error (the flag did not exist), and given
# only ci.yml the gate reported SEVEN confident-looking FALSE violations — the seven nightly-tier
# gates, whose jobs live in bench-nightly.yml. A gate that cries wolf gets ignored.


def test_main_accepts_repo_root():
    """PLANT (c): delete the --repo-root argument from main() and this must RED."""
    assert check_fleet_manifest.main(["--repo-root", str(REPO_ROOT)]) == 0


def test_main_bare_run_resolves_both_default_workflows(monkeypatch):
    """No flags at all: the default set covers BOTH workflows, so the nightly gates verify."""
    monkeypatch.chdir(REPO_ROOT)
    assert check_fleet_manifest.main([]) == 0


def test_main_given_only_ci_yml_no_longer_reports_the_seven(capsys):
    """The measured friction: naming ONLY ci.yml must not manufacture the seven nightly violations,
    because the gate resolves bench-nightly.yml from its own default set."""
    rc = check_fleet_manifest.main(["--repo-root", str(REPO_ROOT),
                                    "--ci-workflow", str(LIVE_WORKFLOW)])
    assert rc == 0
    assert "bench-100k-nightly" not in capsys.readouterr().err


def test_main_default_workflow_set_missing_file_is_config_error(tmp_path):
    """A default that resolved nothing would look exactly like a manifest whose every job exists, so
    a missing DEFAULT workflow is exit 2 — never a silent skip."""
    (tmp_path / "src").mkdir()
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "ci-fleet-manifest.json").write_text(
        json.dumps(base_manifest()), encoding="utf-8")
    assert check_fleet_manifest.main(["--repo-root", str(tmp_path)]) == 2


def test_main_missing_src_tree_is_config_error(tmp_path):
    """Rule 8's claim C reads the ctest registrations from src/; no src/ is a config error, not a
    pass — a gate that cannot run must not report success."""
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "ci-fleet-manifest.json").write_text(
        json.dumps(base_manifest()), encoding="utf-8")
    for relative in check_fleet_manifest.WORKFLOW_DEFAULTS.values():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(WORKFLOW, encoding="utf-8")
    assert check_fleet_manifest.main(["--repo-root", str(tmp_path)]) == 2


# --- rule 6, now TIER-AWARE -----------------------------------------------------------------------


def test_nightly_gate_pointing_at_a_per_pr_job_is_flagged():
    """A nightly gate whose ci_job_id names a REAL job that lives in the PER-PR workflow used to pass
    (the two files were read as one pool). The tier decides which file must carry it."""
    m = base_manifest()
    m["gates"][1]["ci_job_id"] = "build"  # a real ci.yml job, but this row is tier=nightly
    errors = check_fleet_manifest.validate(
        m, workflows={"per-PR": WORKFLOW, "nightly": "jobs:\n  bench-100k-nightly:\n"})
    assert any("has no matching job in the nightly workflow" in e for e in errors)


def test_tier_aware_rule_6_accepts_a_correctly_placed_nightly_job():
    m = base_manifest()
    m["gates"][1]["ci_job_id"] = "bench-100k-nightly"
    assert check_fleet_manifest.validate(
        m, workflows={"per-PR": WORKFLOW, "nightly": "jobs:\n  bench-100k-nightly:\n"}) == []


# --- rule 8: the prose STATUS-CLAIM drift gate (M9 x11) -------------------------------------------
# The shape that actually bit us: after x7 fixed #437 and e12c-1 landed, the editor-shell-cef-smoke
# row still asserted "macOS ctest registration DISABLED pending #437" while CI stayed green — a FALSE
# status claim that survived a FULL TASK CYCLE inside a CI-validated file.

REGISTRATIONS_NONE_DISABLED = ({"editor-shell-cef-staging", "editor-shell-smoke"}, set())
REGISTRATIONS_ONE_DISABLED = (
    {"editor-shell-cef-staging", "editor-shell-smoke"}, {"editor-shell-smoke"})


def _with_description(text: str, **fields) -> dict:
    m = base_manifest()
    m["gates"][0]["description"] = text
    m["gates"][0].update(fields)
    return m


def test_claim_parked_on_issue_must_be_recorded_in_the_issue_field():
    """THE MEASURED DEFECT, reconstructed: a blocking row claiming it is pending an issue it does not
    name."""
    errors = check_fleet_manifest.validate(
        _with_description("macOS ctest registration DISABLED pending #437."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert any("does not name #437" in e for e in errors)


def test_claim_parked_on_issue_is_clean_when_the_row_records_it():
    errors = check_fleet_manifest.validate(
        _with_description("Quarantined awaiting #437.",
                          red_x_policy="quarantine-with-issue", issue="#437"),
        WORKFLOW, registrations=REGISTRATIONS_NONE_DISABLED)
    assert errors == []


def test_claim_awaiting_provision_of_an_already_provisioned_class():
    errors = check_fleet_manifest.validate(
        _with_description("Numbers advisory until gh-ubuntu-shared is provisioned."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert any("already declared provisioned=True" in e for e in errors)


def test_claim_awaiting_provision_of_an_undeclared_class():
    errors = check_fleet_manifest.validate(
        _with_description("Advisory until perf-box-typo is provisioned."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert any("is not declared in runner_classes" in e for e in errors)


def test_claim_awaiting_provision_is_clean_for_an_unprovisioned_class():
    errors = check_fleet_manifest.validate(
        _with_description("Advisory until perf-box is provisioned."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert errors == []


def test_claim_awaiting_provision_ignores_the_bare_words_runner_and_class():
    """FALSE-POSITIVE CONTROL, from the live measurement: 4 of the 8 live matches capture the bare
    word `class` or `runner` out of 'until the runner class is provisioned'. Neither is a runner-class
    id, and flagging them would make this rule cry wolf on correct prose."""
    for text in ("Advisory until the runner class is provisioned.",
                 "The floor gate activates when its runner class is provisioned."):
        assert check_fleet_manifest.validate(
            _with_description(text), WORKFLOW,
            registrations=REGISTRATIONS_NONE_DISABLED) == []


def test_claim_disabled_naming_a_registered_ctest_that_is_not_disabled():
    errors = check_fleet_manifest.validate(
        _with_description("The ctest editor-shell-cef-staging is currently DISABLED."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert any("no such ctest carries a DISABLED property" in e for e in errors)


def test_claim_disabled_naming_a_registered_ctest_that_really_is_disabled():
    errors = check_fleet_manifest.validate(
        _with_description("The ctest editor-shell-smoke is currently DISABLED."), WORKFLOW,
        registrations=REGISTRATIONS_ONE_DISABLED)
    assert errors == []


def test_claim_disabled_with_no_named_ctest_falls_back_to_the_whole_tree():
    errors = check_fleet_manifest.validate(
        _with_description("Its registration ships DISABLED on macOS."), WORKFLOW,
        registrations=REGISTRATIONS_NONE_DISABLED)
    assert any("NO ctest in the tree carries a DISABLED property" in e for e in errors)


def test_historical_narrative_about_a_removed_disabled_property_stays_green():
    """THE LOAD-BEARING FALSE-POSITIVE CONTROL. e12c-2's CORRECTED text mentions DISABLED while
    claiming the OPPOSITE, so a keyword match here would fail the very row that was fixed — the
    patterns are ASSERTION shapes, not keywords."""
    errors = check_fleet_manifest.validate(
        _with_description(
            "M9 x7 (issue #437) removed the two DISABLED properties it had shipped them with."),
        WORKFLOW, registrations=REGISTRATIONS_NONE_DISABLED)
    assert errors == []


def test_rule_8_skips_claim_c_when_registrations_were_not_supplied():
    """Claim C needs the sources; the legacy two-argument validate() call has none. A and B still run
    — which is why main() always supplies registrations rather than letting C be skipped in CI."""
    errors = check_fleet_manifest.validate(
        _with_description("Its registration ships DISABLED on macOS."), WORKFLOW)
    assert errors == []


def test_live_manifest_has_no_status_claim_drift():
    """The committed manifest must be clean under rule 8 — measured over all 94 rows before the rule
    was enabled (claims A and C hit zero rows, B hits eight, all consistent)."""
    manifest = json.loads(LIVE_MANIFEST.read_text(encoding="utf-8"))
    registrations = check_fleet_manifest.ctest_registrations(REPO_ROOT / "src")
    workflows = {tier: (REPO_ROOT / relative).read_text(encoding="utf-8")
                 for tier, relative in check_fleet_manifest.WORKFLOW_DEFAULTS.items()}
    assert check_fleet_manifest.validate(
        manifest, workflows=workflows, registrations=registrations) == []


def test_ctest_registrations_reads_the_live_tree():
    """Anti-vacuity for rule 8's own input: an empty registration set would make every claim-C check
    on a NAMED ctest fall through to the tree-wide branch."""
    registered, disabled = check_fleet_manifest.ctest_registrations(REPO_ROOT / "src")
    assert "editor-shell-cef-staging" in registered
    assert len(registered) > 100
    assert disabled == set(), "no ctest carries a DISABLED property today (x7 removed the last two)"
