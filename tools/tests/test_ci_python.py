"""Tests for tools/check_ci_python.py — every CI job installs its own Python (owner rule).

Each rule is exercised by PLANTING its violation into a workflow the gate accepts, so every case is
known to change the verdict; then the LIVE committed workflows are run through it.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

check_ci_python = load_tool("check_ci_python")

REPO_ROOT = Path(__file__).resolve().parents[2]

GOOD_JOB = (
    "  build:\n"
    "    runs-on: ubuntu-latest\n"
    "    defaults:\n"
    "      run:\n"
    "        shell: bash\n"
    "    steps:\n"
    "      - uses: actions/checkout@v7\n"
    "\n"
    "      # comment\n"
    "      - name: Python (installed by the workflow)\n"
    "        uses: ./.github/actions/python\n"
    "\n"
    "      - name: Toolchain\n"
    "        uses: ./.github/actions/pinned-toolchain\n"
    "      - run: python3 tools/x.py\n"
)


def make_repo(tmp_path: Path, jobs: str, extra_action: str | None = None) -> Path:
    wf = tmp_path / ".github" / "workflows"
    wf.mkdir(parents=True)
    (wf / "ci.yml").write_text("name: CI\non:\n  push:\n\njobs:\n" + jobs, encoding="utf-8")
    if extra_action is not None:
        act = tmp_path / ".github" / "actions" / "x"
        act.mkdir(parents=True)
        (act / "action.yml").write_text(extra_action, encoding="utf-8")
    return tmp_path


def run(root: Path) -> int:
    return check_ci_python.main(["--repo-root", str(root), "--min-jobs", "1"])


def test_compliant_job_passes(tmp_path):
    assert run(make_repo(tmp_path, GOOD_JOB)) == 0


def test_missing_python_step_reds(tmp_path, capsys):
    job = GOOD_JOB.replace("        uses: ./.github/actions/python\n", "        run: echo hi\n")
    assert run(make_repo(tmp_path, job)) == 1
    assert "job 'build'" in capsys.readouterr().err


def test_run_step_before_python_reds(tmp_path):
    job = GOOD_JOB.replace("      - uses: actions/checkout@v7\n",
                           "      - uses: actions/checkout@v7\n      - run: python3 x.py\n")
    assert run(make_repo(tmp_path, job)) == 1


def test_local_action_before_python_reds(tmp_path):
    job = GOOD_JOB.replace("      - uses: actions/checkout@v7\n",
                           "      - uses: actions/checkout@v7\n"
                           "      - uses: ./.github/actions/pinned-toolchain\n")
    assert run(make_repo(tmp_path, job)) == 1


def test_job_level_defaults_run_is_not_a_step(tmp_path):
    # The GOOD_JOB fixture already carries `defaults: run:` ahead of `steps:`; it must stay green.
    assert "    defaults:\n      run:\n" in GOOD_JOB
    assert run(make_repo(tmp_path, GOOD_JOB)) == 0


def test_second_job_without_python_reds(tmp_path, capsys):
    second = "  other:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v7\n"
    assert run(make_repo(tmp_path, GOOD_JOB + "\n" + second)) == 1
    assert "job 'other'" in capsys.readouterr().err


def test_setup_python_anywhere_reds(tmp_path, capsys):
    action = "runs:\n  using: composite\n  steps:\n    - uses: actions/setup-python@v7\n"
    assert run(make_repo(tmp_path, GOOD_JOB, extra_action=action)) == 1
    assert "actions/setup-python" in capsys.readouterr().err


def test_unreadable_job_key_is_a_config_error(tmp_path):
    assert run(make_repo(tmp_path, GOOD_JOB + "  bad key here:\n")) == 2


def test_no_workflows_is_a_config_error(tmp_path):
    assert run(tmp_path) == 2


def test_sanity_floor_refuses_a_vacuous_ok(tmp_path):
    root = make_repo(tmp_path, GOOD_JOB)
    assert check_ci_python.main(["--repo-root", str(root), "--min-jobs", "2"]) == 2


def test_live_workflows_comply():
    errors, total = check_ci_python.check(REPO_ROOT)
    assert errors == []
    assert total >= check_ci_python.MIN_JOBS
    assert check_ci_python.main(["--repo-root", str(REPO_ROOT)]) == 0
