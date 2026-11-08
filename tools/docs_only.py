#!/usr/bin/env python3
"""CI classifier: does this change touch only documentation?

  python3 tools/docs_only.py

Writes `docs_only=true|false` to $GITHUB_OUTPUT; ci.yml skips its heavy jobs only on an explicit
`true`. So every doubt means a full run: a path off the list, an empty diff, a commit range this
cannot prove, or a failure here, since the step is `continue-on-error` and leaves the output empty.
A failed checkout must not classify some other tree, so HEAD must be GITHUB_SHA.

Reads the diff of the pull request's merge commit (HEAD^1 is the base), or of a pushed commit when it
is the only one in the push (HEAD^1 is the push's `before`). That needs `fetch-depth: 2` and no token
or API. `--no-renames` because a rename from src/ into docs/ would otherwise list only its docs/ side.

To skip CI for more paths, edit is_docs_path and its table in tests/test_docs_only.py. List only what
no build, test, packaging or generation step reads. Not on the list although they look like docs:
LICENSE.md (CPack reads it), `*.md` outside docs/ (package READMEs live with the code). README.md and
SECURITY.md have no reader today; no recent PR changed only them, so they are not worth the risk yet.
"""

from __future__ import annotations

import os
import subprocess
import sys
from collections.abc import Sequence

# Files under docs/ that a build or test reads, so changing one must run everything.
READ_BY_CI = frozenset({
    "docs/ci-fleet-manifest.json",  # check_fleet_manifest.py, bench/minspec_floor.py, their tests
    "docs/density-targets.md",  # m85-exit-5 (src/tests/integration/CMakeLists.txt)
    "docs/ui-capability-matrix.md",  # test_capability_matrix (src/packages/ui/CMakeLists.txt)
})


def is_docs_path(path: str) -> bool:
    """`path` is repo-relative with forward slashes, as git prints it."""
    if path == "AGENTS.md" or path.startswith(".taskflow/"):
        return True
    return path.startswith("docs/") and path not in READ_BY_CI


def is_docs_only(files: Sequence[str]) -> bool:
    return bool(files) and all(map(is_docs_path, files))


def _git(repo: str, *args: str) -> str:
    return subprocess.run(("git", *args), cwd=repo, check=True, capture_output=True,
                          encoding="utf-8").stdout


def changed_files(repo: str, event: str | None, before: str | None) -> list[str]:
    """The files the commit under test changes against its base. Raises when the range is unproven."""
    if event == "pull_request":
        _git(repo, "rev-parse", "--verify", "HEAD^2")  # raises unless HEAD is the merge commit
    elif _git(repo, "rev-parse", "HEAD^1").strip() != before:
        raise ValueError(f"the {event} holds more than HEAD: before={before}")
    diff = _git(repo, "diff", "--name-only", "--no-renames", "-z", "HEAD^1", "HEAD")
    return [path for path in diff.split("\0") if path]


def main() -> int:
    repo = os.getcwd()
    head = _git(repo, "rev-parse", "HEAD").strip()
    if head != os.environ.get("GITHUB_SHA"):
        raise ValueError(f"HEAD {head} is not GITHUB_SHA {os.environ.get('GITHUB_SHA')}")
    files = changed_files(repo, os.environ.get("GITHUB_EVENT_NAME"), os.environ.get("PUSH_BEFORE"))
    others = [path for path in files if not is_docs_path(path)]
    docs_only = "true" if is_docs_only(files) else "false"
    example = f", e.g. {others[0]}" if others else ""
    print(f"{len(files)} changed file(s), {len(others)} off the docs list{example}")
    print(f"docs_only={docs_only}")
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"docs_only={docs_only}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
