#!/usr/bin/env python3
"""CI gating-topology gate: which jobs may FRONT the rollup via `needs:` (issue #459).

WHY THIS EXISTS. `.github/workflows/ci.yml` fronts ~20 heavyweight job definitions (~41 jobs after
the OS matrices) behind a couple of ~10-second gate jobs via `needs:`, so an obviously broken tree
stops before it burns the runner pool. That fail-fast is worth keeping. What is NOT safe is putting a
job with TIMING flake surface in that position: a `needs:` edge does not merely red the rollup, it
SKIPS every dependent job, and the only recovery is a full rerun of all ~41.

MEASURED (#459): `python-tests` ran the whole `tools/tests` + `bench/tests` pytest suite — which binds
ephemeral ports, spawns real subprocesses, waits on `threading.Event`s and reaps process groups — and
sat in every heavy job's `needs:`. One racy assertion in a Python tooling unit test
(`test_collector_receives_frames_and_done`, which sampled an Event another thread sets after writing
its HTTP response) therefore skipped the entire rollup, repeatedly, all milestone. It survived because
each occurrence "cleared on a rerun" and so read as cheap.

So the invariant this gate pins is not "less gating" — it is a DECLARED split:

  * GATE_JOBS      — allowed to front the rollup. Each must be DETERMINISTIC: no network, no
                     subprocess, no timing, no ports. That property is what makes a `needs:` edge on
                     it safe, and it is asserted by review, not by this script (a scanner cannot prove
                     a program is timing-free) — which is exactly why the set is a short, explicit,
                     reviewed allowlist here rather than "whatever ci.yml happens to say".
  * NON_GATING_JOBS — jobs that MUST NOT appear in any `needs:`. They stay blocking required checks,
                     so a red still reds the run; it just costs a 1-job rerun instead of 41.
  * every other job — must carry EXACTLY the GATE_JOBS edges, so the fail-fast cannot be silently
                     deleted job-by-job either. Deleting gating is the opposite failure of over-gating
                     and this gate refuses both directions.

Rules (each violation names the job and the fix):
  1. Every name in any `needs:` is a real job in the file (typo guard — GitHub silently never runs a
     job whose `needs:` names nothing, so a typo here is invisible).
  2. No job's `needs:` contains a NON_GATING_JOBS member.
  3. Every job that is neither a gate nor non-gating carries exactly the GATE_JOBS set.
  4. A gate job itself declares no `needs:` (a gate behind a gate is not a fail-fast).
  5. Both declared sets exist in the file, so a rename cannot turn a rule into a no-op — the vacuity
     failure mode for a gate keyed on names.

Exit 0 = topology valid. Exit 1 = violations (listed on stdout). Exit 2 = configuration error
(unreadable workflow, no `jobs:` block) — never a silent pass.

The workflow is parsed with a deliberately narrow hand-rolled reader rather than a YAML library:
PyYAML is not a declared dependency of this repo (it would need a `tools/license-allowlist.json`
entry), and `tools/check_fleet_manifest.py` already reads these same workflows by pattern. The reader
is exercised against the REAL ci.yml by this gate's own pytest, so it cannot drift into accepting a
shape the file does not use.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TAG = "ci-gating"

# The DETERMINISTIC gates allowed to front the rollup. Adding a name here is a claim that the job has
# no network / subprocess / timing / port dependence — see the module docstring.
GATE_JOBS = ("license-gate", "ci-config-gate")

# Jobs forbidden from EVER appearing in a `needs:` list, because they do have timing flake surface.
NON_GATING_JOBS = ("python-tests",)


def parse_jobs(text: str) -> dict[str, list[str]]:
    """Map each top-level job id in a workflow to its declared `needs:` list.

    Narrow by design: job ids are the 2-space-indented keys under a column-0 `jobs:`, and `needs:` is
    read at any deeper indent within the job, in either the flow form (`needs: [a, b]`) or the block
    form (`needs:` then `- a`). A job with no `needs:` maps to [].
    """
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines) if re.match(r"^jobs:\s*$", ln))
    except StopIteration:
        raise ValueError("no top-level `jobs:` block")

    jobs: dict[str, list[str]] = {}
    current: str | None = None
    pending_block = False
    for raw in lines[start + 1:]:
        line = raw.rstrip("\n")
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        # A column-0 key ends the jobs block.
        if re.match(r"^\S", line):
            break
        job = re.match(r"^  (?P<id>[A-Za-z0-9_-]+):\s*$", line)
        if job:
            current = job.group("id")
            jobs[current] = []
            pending_block = False
            continue
        if current is None:
            continue
        flow = re.match(r"^\s+needs:\s*\[(?P<items>[^\]]*)\]\s*$", line)
        if flow:
            jobs[current] = [n.strip().strip("'\"") for n in flow.group("items").split(",")
                             if n.strip()]
            pending_block = False
            continue
        scalar = re.match(r"^\s+needs:\s*(?P<one>[A-Za-z0-9_-]+)\s*$", line)
        if scalar:
            jobs[current] = [scalar.group("one")]
            pending_block = False
            continue
        if re.match(r"^\s+needs:\s*$", line):
            jobs[current] = []
            pending_block = True
            continue
        if pending_block:
            item = re.match(r"^\s+-\s*(?P<one>[A-Za-z0-9_-]+)\s*$", line)
            if item:
                jobs[current].append(item.group("one"))
                continue
            pending_block = False
    if not jobs:
        raise ValueError("`jobs:` block declares no jobs")
    return jobs


def validate(jobs: dict[str, list[str]]) -> list[str]:
    """Return a list of human-readable violations (empty == valid)."""
    errors: list[str] = []
    gates = set(GATE_JOBS)
    non_gating = set(NON_GATING_JOBS)

    # Rule 5 FIRST: a renamed job would otherwise turn rules 2-4 into silent no-ops.
    for name in (*GATE_JOBS, *NON_GATING_JOBS):
        if name not in jobs:
            errors.append(
                f"declared job {name!r} does not exist in the workflow — this gate's rules would "
                f"silently pass. Update GATE_JOBS / NON_GATING_JOBS in tools/check_ci_gating.py "
                f"together with the rename.")

    for job, needs in sorted(jobs.items()):
        # Rule 1: no dangling edge.
        for dep in needs:
            if dep not in jobs:
                errors.append(f"job {job!r}: needs {dep!r}, which is not a job in this workflow")

        # Rule 2: the timing-dependent jobs may never gate anything.
        for dep in sorted(set(needs) & non_gating):
            errors.append(
                f"job {job!r}: must NOT declare `needs: {dep}` — {dep!r} has timing flake surface, "
                f"so that edge lets one flaky test SKIP this job (issue #459). Keep {dep!r} a "
                f"blocking check with no dependents.")

        if job in gates:
            # Rule 4: gates front everything and wait for nothing.
            if needs:
                errors.append(
                    f"gate job {job!r}: must declare no `needs:` (found {sorted(needs)}) — a gate "
                    f"behind another job is not a fail-fast.")
            continue
        if job in non_gating:
            continue

        # Rule 3: the retained fail-fast must be present, and nothing else may gate.
        missing = gates - set(needs)
        extra = set(needs) - gates
        if missing:
            errors.append(
                f"job {job!r}: missing the fail-fast gate edge(s) {sorted(missing)} — every heavy "
                f"job is fronted by {sorted(gates)} so a broken tree cannot burn the runner pool.")
        if extra:
            errors.append(
                f"job {job!r}: unexpected `needs:` entr(y/ies) {sorted(extra)} — only the "
                f"deterministic gates {sorted(gates)} may front the rollup. If a new gate is "
                f"genuinely deterministic, add it to GATE_JOBS in tools/check_ci_gating.py with the "
                f"reasoning.")
    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="repository root (default: .)")
    ap.add_argument("--workflow", default=None,
                    help="workflow to check (default: <repo-root>/.github/workflows/ci.yml)")
    args = ap.parse_args(argv)

    path = (Path(args.workflow) if args.workflow
            else Path(args.repo_root) / ".github" / "workflows" / "ci.yml")
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"[{TAG}] ERROR: cannot read {path}: {exc}", file=sys.stderr)
        return 2
    try:
        jobs = parse_jobs(text)
    except ValueError as exc:
        print(f"[{TAG}] ERROR: {path}: {exc}", file=sys.stderr)
        return 2

    errors = validate(jobs)
    if errors:
        print(f"[{TAG}] FAIL: {len(errors)} gating-topology violation(s) in {path}:")
        for err in errors:
            print(f"[{TAG}]   - {err}")
        return 1
    gated = sum(1 for j, n in jobs.items()
                if j not in set(GATE_JOBS) | set(NON_GATING_JOBS) and n)
    print(f"[{TAG}] OK: {len(jobs)} job(s); {gated} fronted by {list(GATE_JOBS)}; "
          f"{list(NON_GATING_JOBS)} gate nothing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
