"""Tests for tools/check_ci_gating.py — the #459 gating-topology gate.

The gate's whole value is that it goes RED when someone re-fronts the rollup behind a
timing-dependent job, or quietly deletes the fail-fast from one job. So per R-QA-013 every rule is
exercised against a synthetic workflow violating exactly that one rule (a rule that cannot be made to
fail is not evidence), alongside the shapes it must NOT flag — and then the LIVE committed ci.yml is
run through it, which is what stops the hand-rolled reader from drifting into accepting a shape the
real file does not use.

Every case below was authored by PLANTING the violation into a workflow the gate already accepted, so
each one is known to change the verdict rather than merely to be present.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

check_ci_gating = load_tool("check_ci_gating")

parse_jobs = check_ci_gating.parse_jobs
validate = check_ci_gating.validate
GATE_JOBS = check_ci_gating.GATE_JOBS
NON_GATING_JOBS = check_ci_gating.NON_GATING_JOBS

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_CI = REPO_ROOT / ".github" / "workflows" / "ci.yml"


def workflow(*, heavy_needs: str = "[license-gate, ci-config-gate]",
             gate_needs: str = "", extra: str = "") -> str:
    """A minimal but structurally REAL ci.yml: the two gates, the non-gating pytest job, one heavy
    job. `gate_needs` / `heavy_needs` / `extra` are the plant seams."""
    gate_line = f"    needs: {gate_needs}\n" if gate_needs else ""
    return (
        "name: CI\n"
        "\n"
        "on:\n"
        "  pull_request:\n"
        "\n"
        "env:\n"
        "  FOO: bar\n"
        "\n"
        "jobs:\n"
        "  license-gate:\n"
        "    name: dependency-license gate\n"
        "    runs-on: ubuntu-latest\n"
        f"{gate_line}"
        "    steps:\n"
        "      - uses: actions/checkout@v7\n"
        "\n"
        "  ci-config-gate:\n"
        "    name: CI config gate\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - uses: actions/checkout@v7\n"
        "\n"
        "  python-tests:\n"
        "    name: python tests\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - uses: actions/checkout@v7\n"
        "\n"
        "  build:\n"
        "    name: build (${{ matrix.os }})\n"
        f"    needs: {heavy_needs}\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - uses: actions/checkout@v7\n"
        f"{extra}"
    )


# ---------------------------------------------------------------------------
# The reader
# ---------------------------------------------------------------------------


def test_parse_jobs_reads_flow_and_absent_needs():
    jobs = parse_jobs(workflow())
    assert set(jobs) == {"license-gate", "ci-config-gate", "python-tests", "build"}
    assert jobs["build"] == ["license-gate", "ci-config-gate"]
    assert jobs["license-gate"] == []


def test_parse_jobs_reads_block_and_scalar_needs():
    jobs = parse_jobs(workflow(heavy_needs="\n      - license-gate\n      - ci-config-gate"))
    assert jobs["build"] == ["license-gate", "ci-config-gate"]
    assert parse_jobs(workflow(heavy_needs="license-gate"))["build"] == ["license-gate"]


def test_parse_jobs_ignores_env_keys_and_stops_at_column_zero():
    """`env:`/`on:` blocks precede `jobs:`, and a trailing column-0 key must end it — otherwise a
    later top-level mapping's children would be read as jobs."""
    jobs = parse_jobs(workflow() + "\nconcurrency:\n  group: x\n  cancel-in-progress: true\n")
    assert "group" not in jobs and "FOO" not in jobs


def test_parse_jobs_rejects_a_workflow_with_no_jobs_block():
    with pytest.raises(ValueError):
        parse_jobs("name: CI\non:\n  pull_request:\n")


# ---------------------------------------------------------------------------
# The rules — one synthetic violation each
# ---------------------------------------------------------------------------


def test_the_reference_topology_is_accepted():
    assert validate(parse_jobs(workflow())) == []


def test_rule1_dangling_needs_edge_is_flagged():
    errors = validate(parse_jobs(workflow(heavy_needs="[license-gate, ci-config-gaet]")))
    assert any("not a job in this workflow" in e for e in errors)


def test_rule2_a_non_gating_job_may_not_gate_anything():
    """THE defect this gate exists for: re-adding the timing-dependent pytest job to a `needs:`."""
    errors = validate(parse_jobs(
        workflow(heavy_needs="[license-gate, ci-config-gate, python-tests]")))
    assert any("must NOT declare `needs: python-tests`" in e for e in errors)


def test_rule3_a_deleted_fail_fast_edge_is_flagged():
    """The OPPOSITE failure: quietly dropping the gating that is there for a real reason."""
    errors = validate(parse_jobs(workflow(heavy_needs="[ci-config-gate]")))
    assert any("missing the fail-fast gate edge(s) ['license-gate']" in e for e in errors)

    errors = validate(parse_jobs(workflow(heavy_needs="[license-gate]")))
    assert any("missing the fail-fast gate edge(s) ['ci-config-gate']" in e for e in errors)


def test_rule3_an_undeclared_gate_is_flagged():
    """A new job may not become a gate by appearing in a `needs:` — the allowlist is the decision."""
    extra = ("\n  smoke-gate:\n"
             "    runs-on: ubuntu-latest\n"
             "    steps:\n"
             "      - uses: actions/checkout@v7\n")
    errors = validate(parse_jobs(
        workflow(heavy_needs="[license-gate, ci-config-gate, smoke-gate]", extra=extra)))
    assert any("unexpected `needs:` entr" in e and "smoke-gate" in e for e in errors)


def test_rule4_a_gate_behind_another_job_is_flagged():
    errors = validate(parse_jobs(workflow(gate_needs="[ci-config-gate]")))
    assert any("must declare no `needs:`" in e for e in errors)


def test_rule5_a_renamed_declared_job_cannot_silently_pass():
    """Vacuity guard: with GATE_JOBS/NON_GATING_JOBS naming a job that no longer exists, rules 2-4
    would match nothing and the gate would print OK across any topology."""
    text = workflow().replace("  python-tests:\n", "  py-tests:\n")
    errors = validate(parse_jobs(text))
    assert any("does not exist in the workflow" in e and "python-tests" in e for e in errors)


# ---------------------------------------------------------------------------
# The LIVE workflow
# ---------------------------------------------------------------------------


def test_live_ci_yml_topology_is_valid():
    jobs = parse_jobs(LIVE_CI.read_text(encoding="utf-8"))
    assert validate(jobs) == []


def test_live_ci_yml_is_actually_gated_and_the_reader_saw_it():
    """Guards the reader itself: a parser that silently returned `[]` for every `needs:` would make
    rules 1-2 vacuous and rule 3 red, so assert the live file really is fronted."""
    jobs = parse_jobs(LIVE_CI.read_text(encoding="utf-8"))
    for name in (*GATE_JOBS, *NON_GATING_JOBS):
        assert name in jobs, f"{name} missing from the live workflow"
    fronted = [j for j, n in jobs.items() if set(GATE_JOBS) <= set(n)]
    assert len(fronted) >= 15, f"only {len(fronted)} job(s) parsed as fronted — reader drift?"
    assert not any(set(NON_GATING_JOBS) & set(n) for n in jobs.values())


def test_main_exit_codes(tmp_path):
    ok = tmp_path / "ok.yml"
    ok.write_text(workflow(), encoding="utf-8")
    assert check_ci_gating.main(["--workflow", str(ok)]) == 0

    bad = tmp_path / "bad.yml"
    bad.write_text(workflow(heavy_needs="[license-gate, ci-config-gate, python-tests]"),
                   encoding="utf-8")
    assert check_ci_gating.main(["--workflow", str(bad)]) == 1

    assert check_ci_gating.main(["--workflow", str(tmp_path / "nope.yml")]) == 2

    garbage = tmp_path / "garbage.yml"
    garbage.write_text("name: CI\n", encoding="utf-8")
    assert check_ci_gating.main(["--workflow", str(garbage)]) == 2
