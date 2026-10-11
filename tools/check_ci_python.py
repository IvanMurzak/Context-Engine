#!/usr/bin/env python3
"""CI Python gate: every workflow job installs its OWN Python before it runs anything.

WHY THIS EXISTS. Owner rule: a GitHub Actions workflow is self-sufficient — it installs the Python it
needs instead of trusting whatever the runner machine has. The Windows legs used to only PROBE the
self-hosted box for an interpreter (`.github/actions/windows-python`), so the day that box's Python
install broke, every Windows leg went red with nothing in this repo having changed. Every job now
runs `.github/actions/python` (uv-managed CPython, no registry writes) right after checkout; this gate
keeps it that way (docs/self-hosted-runners.md § 6).

Rules (each violation names the file, the job and the fix):
  1. Every job in every `.github/workflows/*.yml` has a step `uses: ./.github/actions/python`.
  2. That step precedes the job's first `run:` step and its first other local composite action
     (those run Python too: pinned-toolchain, setup-build-env, …), so nothing uses a machine Python.
  3. Nothing under `.github/` uses `actions/setup-python`: on a LocalSystem Windows runner it runs the
     CPython installer as administrator, which rewrites the machine-global
     HKLM\\SOFTWARE\\Python\\PythonCore key — the 2026-08-27/29 outages (§ 6).

Exit 0 = all jobs comply. Exit 1 = violations. Exit 2 = configuration error (no workflows, a workflow
with no `jobs:`, or fewer jobs than the sanity floor) — never a silent pass. Stdlib-only, a pure read
of committed files: safe to run in the deterministic `ci-config-gate`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TAG = "ci-python"
PYTHON_ACTION = "./.github/actions/python"

_JOB_KEY = re.compile(r"""^  ['"]?(?P<id>[A-Za-z0-9_-]+)['"]?:\s*(?:#.*)?$""")
_USES = re.compile(r"""^\s+(?:-\s+)?uses:\s*['"]?(?P<ref>[^\s'"#]+)""")
_RUN = re.compile(r"^\s+(?:-\s+)?run:")
_SETUP_PYTHON = re.compile(r"""^\s*(?:-\s+)?uses:\s*['"]?actions/setup-python@""", re.M)

# Sanity floor for the repo's own workflows (24 + 2 + 3 jobs today): catches a reader COLLAPSE, not a
# job being added or removed.
MIN_JOBS = 20


def parse_jobs(text: str) -> dict[str, list[str]]:
    """Map each top-level job id to its lines (the 2-space keys under a column-0 `jobs:`)."""
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines) if re.match(r"^jobs:\s*(?:#.*)?$", ln))
    except StopIteration:
        raise ValueError("no top-level `jobs:` block")
    jobs: dict[str, list[str]] = {}
    current: str | None = None
    for line in lines[start + 1:]:
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        if re.match(r"^\S", line):
            break
        job = _JOB_KEY.match(line)
        if job:
            current = job.group("id")
            jobs[current] = []
            continue
        if re.match(r"^  \S", line):
            raise ValueError(f"a 2-space key under `jobs:` this reader cannot read as a job id: "
                             f"{line.strip()!r} — refusing rather than skipping a job")
        if current is not None:
            jobs[current].append(line)
    if not jobs:
        raise ValueError("`jobs:` block declares no jobs")
    return jobs


def validate_job(job_lines: list[str]) -> str | None:
    """Return why this job violates rules 1-2, or None when it complies."""
    python_at = None
    # Only the `steps:` list counts: a job-level `defaults: run:` is not a step.
    steps_at = next((i for i, ln in enumerate(job_lines) if re.match(r"^    steps:\s*(?:#.*)?$", ln)),
                    None)
    if steps_at is None:
        return "no `steps:` list"
    for i, line in enumerate(job_lines[steps_at + 1:], start=steps_at + 1):
        uses = _USES.match(line)
        if uses and uses.group("ref") == PYTHON_ACTION:
            python_at = i
            break
        if _RUN.match(line):
            return f"a `run:` step precedes `uses: {PYTHON_ACTION}` (or the step is missing)"
        if uses and uses.group("ref").startswith("./"):
            return (f"local action {uses.group('ref')} precedes `uses: {PYTHON_ACTION}` "
                    f"(or the step is missing)")
    if python_at is None:
        return f"no `uses: {PYTHON_ACTION}` step"
    return None


def check(root: Path) -> tuple[list[str], int]:
    """Return (violations, jobs checked). Raises ValueError on a configuration error."""
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        raise ValueError(f"no workflows under {root / '.github' / 'workflows'}")
    errors: list[str] = []
    total = 0
    for wf in workflows:
        rel = wf.relative_to(root).as_posix()
        try:
            jobs = parse_jobs(wf.read_text(encoding="utf-8"))
        except ValueError as exc:
            raise ValueError(f"{rel}: {exc}") from None
        total += len(jobs)
        for job, body in jobs.items():
            why = validate_job(body)
            if why:
                errors.append(f"{rel} job {job!r}: {why}. Add the step right after "
                              f"actions/checkout so the job installs its own Python.")
    for f in sorted((root / ".github").rglob("*.y*ml")):
        if _SETUP_PYTHON.search(f.read_text(encoding="utf-8")):
            errors.append(f"{f.relative_to(root).as_posix()}: uses actions/setup-python, which "
                          f"rewrites HKLM PythonCore on a LocalSystem Windows runner "
                          f"(docs/self-hosted-runners.md § 6) — use {PYTHON_ACTION}.")
    return errors, total


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="repository root (default: .)")
    ap.add_argument("--min-jobs", type=int, default=MIN_JOBS,
                    help="sanity floor on the number of jobs read (TEST SEAM)")
    args = ap.parse_args(argv)
    try:
        errors, total = check(Path(args.repo_root))
    except (ValueError, OSError) as exc:
        print(f"[{TAG}] ERROR: {exc}", file=sys.stderr)
        return 2
    if errors:
        for err in errors:
            print(f"[{TAG}] FINDING: {err}", file=sys.stderr)
        print(f"[{TAG}] FAIL: {len(errors)} violation(s)", file=sys.stderr)
        return 1
    if total < args.min_jobs:
        print(f"[{TAG}] ERROR: read only {total} job(s), below the sanity floor ({args.min_jobs}) — "
              f"the reader is broken; refusing to report a vacuous OK.", file=sys.stderr)
        return 2
    print(f"[{TAG}] OK: all {total} job(s) install their own Python via {PYTHON_ACTION}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
