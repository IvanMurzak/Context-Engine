"""Tests for tools/docs_only.py — which changes may skip ci.yml's heavy jobs.

The table pins the path list. Every path in RUN either has a real reader or is a near miss; see the
script's docstring. The rest runs real git on a real merge commit, the shape a pull_request run gets.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest
from conftest import TOOLS_DIR, load_tool

docs_only = load_tool("docs_only")

is_docs_path = docs_only.is_docs_path
is_docs_only = docs_only.is_docs_only
changed_files = docs_only.changed_files

REPO_ROOT = TOOLS_DIR.parent

SKIP = [
    "AGENTS.md",
    "docs/signing.md",
    "docs/ci-fleet-manifest.md",  # the prose companion; only the .json is read
    "docs/img/readme/header-dark.svg",
    ".taskflow/2026-07-01-context-engine/core/REQUIREMENTS.md",
    ".taskflow/2026-07-01-context-engine/m9-editor/mock.html",
]
RUN = [
    # Under docs/, read by a build or test step.
    "docs/ci-fleet-manifest.json",
    "docs/density-targets.md",
    "docs/ui-capability-matrix.md",
    # Look like docs; are read, or not worth proving.
    "LICENSE.md",
    "README.md",
    "SECURITY.md",
    "src/AGENTS.md",
    "src/packages/ui/README.md",
    "goldens/README.md",
    "samples/README.md",
    "bench/README.md",
    ".github/CONTRIBUTING.md",
    # Code, and near misses of the prefixes.
    "src/CMakeLists.txt",
    ".github/workflows/ci.yml",
    "tools/docs_only.py",
    "tools/tests/test_docs_only.py",
    ".pipeline/workflows/implement-task/pipeline.yml",
    "docs",
    "documents/a.md",
    "Docs/a.md",
    "agents.md",
    ".taskflow",
    ".taskflow-old/a.md",
]


@pytest.mark.parametrize("path", SKIP)
def test_on_the_list(path):
    assert is_docs_path(path)


@pytest.mark.parametrize("path", RUN)
def test_off_the_list(path):
    assert not is_docs_path(path)


def test_every_exclusion_is_a_real_file():
    """A renamed file would leave its new name on the list, skipping the step that reads it."""
    for path in docs_only.READ_BY_CI:
        assert (REPO_ROOT / path).is_file(), path


def test_is_docs_only_needs_at_least_one_file_and_every_file_on_the_list():
    assert is_docs_only(["docs/a.md", "AGENTS.md"])
    assert not is_docs_only(["docs/a.md", "src/a.cpp"])
    assert not is_docs_only([])


class Repo:
    def __init__(self, path: Path):
        self.path = path
        self.git("init", "-b", "main")

    def git(self, *args: str) -> str:
        return subprocess.run(
            ("git", "-c", "user.name=t", "-c", "user.email=t@t", *args),
            cwd=self.path, check=True, capture_output=True, encoding="utf-8").stdout.strip()

    def write(self, path: str) -> None:
        (self.path / path).parent.mkdir(parents=True, exist_ok=True)
        (self.path / path).write_text(path, encoding="utf-8")

    def commit(self, message: str) -> None:
        self.git("add", ".")
        self.git("commit", "--allow-empty", "-m", message)

    def merge(self, change) -> None:
        """One base commit, then the merge commit of a branch `change` applied to it."""
        self.write("src/a.cpp")
        self.write("src/b.cpp")  # keeps src/ alive, or git reads a rename of a.cpp as moving the folder
        self.commit("base")
        self.git("checkout", "-b", "pr")
        change()
        self.commit("pr")
        self.git("checkout", "main")
        self.write("src/moved-on-main.cpp")
        self.commit("main moved")
        self.git("merge", "--no-ff", "-m", "merge", "pr")


@pytest.fixture
def repo(tmp_path):
    (tmp_path / "repo").mkdir()
    return Repo(tmp_path / "repo")


def test_changed_files_lists_only_what_the_pull_request_changes(repo):
    repo.merge(lambda: repo.write("docs/a.md"))
    assert changed_files(str(repo.path), "pull_request", None) == ["docs/a.md"]


def test_a_rename_from_code_into_docs_lists_both_sides(repo):
    def rename():
        (repo.path / "docs").mkdir()
        (repo.path / "src/a.cpp").rename(repo.path / "docs/a.cpp")
    repo.merge(rename)
    files = changed_files(str(repo.path), "pull_request", None)
    assert sorted(files) == ["docs/a.cpp", "src/a.cpp"]
    assert not is_docs_only(files)


def test_a_non_ascii_docs_path_stays_unquoted(repo):
    repo.merge(lambda: repo.write("docs/заметка.md"))
    assert is_docs_only(changed_files(str(repo.path), "pull_request", None))


def test_an_empty_diff_is_not_docs_only(repo):
    repo.merge(lambda: None)
    assert not is_docs_only(changed_files(str(repo.path), "pull_request", None))


def test_a_pull_request_needs_a_merge_commit(repo):
    repo.commit("base")
    repo.commit("not a merge")
    with pytest.raises(subprocess.CalledProcessError):
        changed_files(str(repo.path), "pull_request", None)


def test_a_push_reads_its_own_range_only_when_it_holds_one_commit(repo):
    repo.commit("base")
    before = repo.git("rev-parse", "HEAD")
    repo.write("docs/a.md")
    repo.commit("one commit")
    assert changed_files(str(repo.path), "push", before) == ["docs/a.md"]

    repo.write("src/c.cpp")
    repo.commit("code")
    repo.write("docs/b.md")
    repo.commit("the push's last commit is docs, but it also holds code")
    with pytest.raises(ValueError):
        changed_files(str(repo.path), "push", before)


def test_a_push_of_a_merge_reads_the_whole_merged_change(repo):
    repo.write("src/a.cpp")
    repo.commit("base")
    before = repo.git("rev-parse", "HEAD")
    repo.git("checkout", "-b", "pr")
    repo.write("src/c.cpp")
    repo.commit("code")
    repo.write("docs/a.md")
    repo.commit("docs on top")
    repo.git("checkout", "main")
    repo.git("merge", "--no-ff", "-m", "merge", "pr")
    assert sorted(changed_files(str(repo.path), "push", before)) == ["docs/a.md", "src/c.cpp"]


@pytest.mark.parametrize("event", ["workflow_dispatch", "schedule", None])
def test_any_other_event_is_unproven(repo, event):
    repo.commit("base")
    repo.commit("next")
    with pytest.raises(ValueError):
        changed_files(str(repo.path), event, None)


def run_script(repo, sha, event="pull_request", before=None):
    output = repo.path.parent / "output.txt"
    env = {**os.environ, "GITHUB_OUTPUT": str(output), "GITHUB_SHA": sha, "GITHUB_EVENT_NAME": event}
    env.pop("PUSH_BEFORE", None)
    if before:
        env["PUSH_BEFORE"] = before
    result = subprocess.run((sys.executable, str(TOOLS_DIR / "docs_only.py")), cwd=repo.path, env=env,
                            capture_output=True, encoding="utf-8", check=False)
    return result.returncode, output.read_text(encoding="utf-8") if output.exists() else None


def test_the_script_writes_true_only_for_the_commit_under_test(repo):
    repo.merge(lambda: repo.write("docs/a.md"))
    code, output = run_script(repo, "0" * 40)
    assert code != 0 and output is None

    code, output = run_script(repo, repo.git("rev-parse", "HEAD"))
    assert code == 0 and output == "docs_only=true\n"


def test_the_script_writes_false_for_a_code_change(repo):
    repo.merge(lambda: repo.write("src/c.cpp"))
    code, output = run_script(repo, repo.git("rev-parse", "HEAD"))
    assert code == 0 and output == "docs_only=false\n"


def test_the_script_writes_nothing_for_an_unproven_push(repo):
    repo.commit("base")
    repo.commit("one")
    repo.write("docs/a.md")
    repo.commit("two")
    head = repo.git("rev-parse", "HEAD")
    code, output = run_script(repo, head, "push", before="f" * 40)
    assert code != 0 and output is None
