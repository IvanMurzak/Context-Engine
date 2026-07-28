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

import re
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
    later top-level mapping's children would be read as jobs.

    ⚠ The trailer's child key is deliberately BARE (`run:`, no inline value). The previous fixture
    used `concurrency:` with `group: x` / `cancel-in-progress: true`, whose inline VALUES the job-key
    regex already rejects via its `:\\s*$` anchor — so the case passed whether or not the column-0
    stop existed and asserted nothing. MEASURED (M9 x12 review): mutating the reader's `break` to a
    `continue` left the old fixture GREEN; with a bare child key the same mutation yields a phantom
    `run` job and reds. `FOO` is a second, weaker check (it sits before `jobs:` and is never scanned).
    """
    jobs = parse_jobs(workflow() + "\ndefaults:\n  run:\n    shell: bash\n")
    assert "run" not in jobs and "FOO" not in jobs
    assert set(jobs) == {"license-gate", "ci-config-gate", "python-tests", "build"}


def test_parse_jobs_reads_quoted_and_commented_block_items():
    """Both are legal YAML and both used to parse as NO edges (a false RED on a job that HAS them)."""
    jobs = parse_jobs(workflow(
        heavy_needs="\n      - 'license-gate'   # the license fail-fast\n      - \"ci-config-gate\""))
    assert jobs["build"] == ["license-gate", "ci-config-gate"]
    assert parse_jobs(workflow(
        heavy_needs="[license-gate, ci-config-gate]  # the two gates"))["build"] == [
            "license-gate", "ci-config-gate"]


def test_parse_jobs_REFUSES_what_it_cannot_read_rather_than_dropping_it():
    """The fail-CLOSED contract. Silently skipping an unreadable job key is a fail-OPEN (validate()
    says nothing about a job it never sees), so the reader raises instead — main() maps that to exit 2.
    """
    with pytest.raises(ValueError, match="cannot read as a job id"):
        parse_jobs(workflow() + '\n  "quoted-job":\n    runs-on: ubuntu-latest\n')
    with pytest.raises(ValueError, match="cannot parse"):
        parse_jobs(workflow(heavy_needs="[license-gate,\n            ci-config-gate]"))
    with pytest.raises(ValueError, match="SECOND `needs:`"):
        parse_jobs(workflow(extra="    needs: [license-gate, ci-config-gate]\n"))


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
    """Guards the reader itself against BOTH drift modes: dropping an EDGE and dropping a whole JOB.

    The expected number of fronted jobs is derived INDEPENDENTLY of `parse_jobs`, by counting the
    literal gate-edge lines in the raw text. That independence is the whole point:

      * a bare floor (`len(fronted) >= 15`) cannot catch a dropped JOB — MEASURED (M9 x12 review):
        mutating the reader to skip every job id containing `cef` or `bench` hid 2 and 4 live jobs
        respectively and this test stayed GREEN, while `validate()` said nothing because it emits
        nothing about a job it never sees (so those jobs were silently UN-gated);
      * and `len(fronted) == len(jobs) - len(declared)` cannot catch it either, because both sides
        fall together when a job disappears.

    Counting the file's own `needs:` lines is the one expectation that does not move when the reader
    breaks. It needs no maintenance as jobs are added.
    """
    text = LIVE_CI.read_text(encoding="utf-8")
    jobs = parse_jobs(text)
    for name in (*GATE_JOBS, *NON_GATING_JOBS):
        assert name in jobs, f"{name} missing from the live workflow"
    fronted = [j for j, n in jobs.items() if set(GATE_JOBS) <= set(n)]
    declared_edges = len(re.findall(r"(?m)^    needs: \[license-gate, ci-config-gate\]\s*$", text))
    assert declared_edges >= 15, (
        f"only {declared_edges} gate-edge line(s) found textually in {LIVE_CI.name} — either the "
        f"rollup shrank drastically or this test's pattern no longer matches how they are written")
    assert len(fronted) == declared_edges, (
        f"{len(fronted)} job(s) parsed as fronted but the file textually declares {declared_edges} "
        f"gate-edge line(s) — the reader dropped a job or an edge")
    assert not any(set(NON_GATING_JOBS) & set(n) for n in jobs.values())


def test_planted_violations_in_the_LIVE_workflow_are_caught():
    """The plants that decide anything run against the REAL file, not only the synthetic fixture.

    The synthetic cases above prove each rule is falsifiable in isolation; these prove the rules still
    fire once the reader is pointed at the 2200-line file it actually guards — the gap that let the
    fail-open below survive.
    """
    live = LIVE_CI.read_text(encoding="utf-8")
    gate_edge = "    needs: [license-gate, ci-config-gate]\n"
    assert gate_edge in live, "the live gate-edge spelling changed; update this plant"
    for label, planted, needle in (
        ("re-add the non-gating job to a needs:",
         live.replace(gate_edge, "    needs: [license-gate, ci-config-gate, python-tests]\n", 1),
         "must NOT declare `needs: python-tests`"),
        ("delete a retained fail-fast edge",
         live.replace(gate_edge, "    needs: [license-gate]\n", 1),
         "missing the fail-fast gate edge(s) ['ci-config-gate']"),
    ):
        assert planted != live, label
        assert any(needle in e for e in validate(parse_jobs(planted))), label


@pytest.mark.parametrize("spelling", [
    "  new-heavy:\n",
    "  new-heavy:  # added by a future task\n",
    '  "new-heavy":\n',
])
def test_an_ungated_job_added_to_the_LIVE_workflow_never_passes(spelling):
    """An UNGATED heavy job must never validate GREEN — in any legal spelling of its key.

    MEASURED (M9 x12 review): the trailing-comment and quoted spellings parsed as NO job at all, so
    `validate()` returned [] and an ungated heavy job shipped GREEN. Only the plain spelling was
    caught. Both hidden spellings are legal YAML, and every job in `ci.yml` already carries a comment
    block, so a trailing comment on the key is a plausible edit rather than a contrived one.

    Either outcome is acceptable now, and both are fail-CLOSED: the reader either sees the job (rule 3
    reds it) or refuses to parse the file at all (ValueError -> exit 2). What must never happen is a
    silent GREEN.
    """
    planted = (LIVE_CI.read_text(encoding="utf-8").rstrip("\n") + "\n\n" + spelling
               + "    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v7\n")
    try:
        errors = validate(parse_jobs(planted))
    except ValueError:
        return   # refused to parse — fail-closed, which main() maps to exit 2
    assert errors, f"an ungated heavy job spelled {spelling!r} validated GREEN"


def test_main_refuses_a_vacuously_small_topology(tmp_path):
    """The sanity floor lives in the SCRIPT, not only in this pytest — and that placement is the point.

    Issue #459 removed `python-tests` from every `needs:` list, so every assertion in this file now
    gates NOTHING. `check_ci_gating.py` is the half that runs in the GATE position. Without a floor
    there, a reader that collapsed to a handful of jobs would print `OK: 3 job(s); 0 fronted` and exit
    0, and all 39 heavy legs would launch behind a vacuous gate while only the non-gating pytest red.
    """
    wf = tmp_path / ".github" / "workflows"
    wf.mkdir(parents=True)
    (wf / "ci.yml").write_text(workflow(), encoding="utf-8")
    assert check_ci_gating.main(["--repo-root", str(tmp_path)]) == 2
    # ...while the --workflow TEST SEAM is deliberately exempt, or every synthetic fixture above
    # would trip the floor instead of exercising the rule it was written for.
    assert check_ci_gating.main(["--workflow", str(wf / "ci.yml")]) == 0


def test_main_rejects_a_bad_repo_root(tmp_path):
    assert check_ci_gating.main(["--repo-root", str(tmp_path / "nope")]) == 2


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
