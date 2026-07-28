#!/usr/bin/env python3
"""CI gating-topology gate: which jobs may FRONT the rollup via `needs:` (issue #459).

WHY THIS EXISTS. `.github/workflows/ci.yml` fronts 21 heavyweight job definitions (39 of the rollup's
42 checks, after the OS matrices) behind a couple of ~10-second gate jobs via `needs:`, so an obviously
broken tree stops before it burns the runner pool. That fail-fast is worth keeping. What is NOT safe is
putting a job with TIMING flake surface in that position: a `needs:` edge does not merely red the
rollup, it SKIPS every dependent job, and the only recovery is a full rerun of all ~39.

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
                     so a red still reds the run; it just costs a 1-job rerun instead of 39.
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

# The DETERMINISTIC gates allowed to front the rollup. Adding a name here is a claim that every CHECK
# the job runs is free of network / subprocess / timing / port dependence — see the module docstring.
# (Scoped to the checks on purpose: `license-gate` also uploads an SBOM artifact, which IS a network
# step. That is a known, pre-#459 residual — not a licence to add new ones.)
GATE_JOBS = ("license-gate", "ci-config-gate")

# Jobs forbidden from EVER appearing in a `needs:` list, because they do have timing flake surface.
NON_GATING_JOBS = ("python-tests",)


# The reader's recognised spellings. Each tolerates an optional trailing `# comment` and optional
# quoting, because BOTH are legal YAML that a future edit will plausibly use — and an unrecognised
# spelling used to be SILENTLY DROPPED, which is this reader's one fail-OPEN class (see parse_jobs).
_JOB_KEY = re.compile(r"^  (?P<id>[A-Za-z0-9_-]+):\s*(?:#.*)?$")
_TWO_SPACE_KEY = re.compile(r"^  \S")
_NEEDS_FLOW = re.compile(r"^\s+needs:\s*\[(?P<items>[^\]]*)\]\s*(?:#.*)?$")
_NEEDS_SCALAR = re.compile(r"""^\s+needs:\s*(?P<one>['"]?[A-Za-z0-9_-]+['"]?)\s*(?:#.*)?$""")
_NEEDS_EMPTY = re.compile(r"^\s+needs:\s*(?:#.*)?$")
_NEEDS_ANY = re.compile(r"^\s+needs\s*:")
_BLOCK_ITEM = re.compile(r"""^\s+-\s*(?P<one>['"]?[A-Za-z0-9_-]+['"]?)\s*(?:#.*)?$""")


def parse_jobs(text: str) -> dict[str, list[str]]:
    """Map each top-level job id in a workflow to its declared `needs:` list.

    Narrow by design: job ids are the 2-space-indented keys under a column-0 `jobs:`, and `needs:` is
    read at any deeper indent within the job, in either the flow form (`needs: [a, b]`) or the block
    form (`needs:` then `- a`). A job with no `needs:` maps to [].

    ⚠ IT FAILS CLOSED, and that is the whole point of the three `raise ValueError`s below (main()
    turns them into exit 2, "never a silent pass"). A narrow reader has two ways to be wrong, and they
    are NOT symmetric: mis-reading a `needs:` list yields [] and rule 3 REDS (loud, safe), but failing
    to recognise a JOB KEY drops the job from the dict entirely — and `validate()` emits nothing about
    a job it never sees, so an UNGATED heavy job passes. That is a fail-OPEN in exactly the direction
    rule 3 exists to close.

    MEASURED against the live ci.yml (M9 x12 review): appending an ungated job spelled
    `  new-heavy:  # added by a future task` — or `  "new-heavy":` — parsed as NO job at all and
    validated GREEN, while the same job spelled `  new-heavy:` correctly REDS. Both spellings are
    legal YAML, and every job in ci.yml already carries a comment block, so a trailing comment on the
    key is a plausible edit. Hence: recognise those spellings, and REFUSE (never skip) any 2-space key
    or any `needs:` line still unrecognised. `^  \\S` is safe as a catch-all because every line at
    exactly 2-space indent inside `jobs:` IS a job key (comments are skipped above; verified against
    the live file).

    Residual limitation, stated rather than hidden: a job block indented 4 spaces instead of 2 is
    legal YAML this reader would read as keys of the PREVIOUS job. The duplicate-`needs:` check below
    catches that whenever the mis-indented job declares `needs:`; a mis-indented job with no `needs:`
    is still invisible. No job in this repo is spelled that way, and `ci.yml` is the only file checked.
    """
    lines = text.splitlines()
    try:
        start = next(i for i, ln in enumerate(lines) if re.match(r"^jobs:\s*$", ln))
    except StopIteration:
        raise ValueError("no top-level `jobs:` block")

    jobs: dict[str, list[str]] = {}
    needs_declared: set[str] = set()
    current: str | None = None
    pending_block = False
    for offset, raw in enumerate(lines[start + 1:], start=start + 2):
        line = raw.rstrip("\n")
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        # A column-0 key ends the jobs block.
        if re.match(r"^\S", line):
            break
        job = _JOB_KEY.match(line)
        if job:
            current = job.group("id")
            jobs[current] = []
            pending_block = False
            continue
        if _TWO_SPACE_KEY.match(line):
            raise ValueError(
                f"line {offset}: a key at 2-space indent inside `jobs:` that this reader cannot read "
                f"as a job id: {line.strip()!r}. Every such key IS a job, so reading on would drop it "
                f"from the topology and let an UNGATED job pass. Fix the spelling or widen _JOB_KEY.")
        if current is None:
            continue

        flow = _NEEDS_FLOW.match(line)
        scalar = None if flow else _NEEDS_SCALAR.match(line)
        empty = None if (flow or scalar) else _NEEDS_EMPTY.match(line)
        if flow or scalar or empty:
            if current in needs_declared:
                raise ValueError(
                    f"line {offset}: a SECOND `needs:` for job {current!r}. A job declares `needs:` "
                    f"at most once, so this is mis-attribution (most likely a mis-indented job block "
                    f"whose keys are being read as {current!r}'s) — refusing rather than overwriting.")
            needs_declared.add(current)
            if flow:
                jobs[current] = [n.strip().strip("'\"") for n in flow.group("items").split(",")
                                 if n.strip()]
                pending_block = False
            elif scalar:
                jobs[current] = [scalar.group("one").strip("'\"")]
                pending_block = False
            else:
                jobs[current] = []
                pending_block = True
            continue
        if _NEEDS_ANY.match(line):
            raise ValueError(
                f"line {offset}: a `needs:` in a form this reader cannot parse: {line.strip()!r} "
                f"(multi-line flow sequences are not supported). Reading on would treat this job as "
                f"having NO dependencies. Put the list on one line or widen the _NEEDS_* patterns.")

        if pending_block:
            item = _BLOCK_ITEM.match(line)
            if item:
                jobs[current].append(item.group("one").strip("'\""))
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
                f"genuinely deterministic (no network, no subprocess, no timing, no ports), add it to "
                f"GATE_JOBS in tools/check_ci_gating.py with the reasoning. If instead this is a "
                f"genuine job-to-job dependency (a job consuming another's ARTIFACT), that is a "
                f"DIFFERENT case this rule does not model — it needs its own documented exception "
                f"here, weighed against the fact that a red upstream will SKIP this job.")
    return errors


# SANITY FLOORS for the repo's own ci.yml, enforced by main() (exit 2), NOT merely printed.
#
# WHY THEY LIVE IN THE SCRIPT and not only in the pytest: the pytest floor
# (test_live_ci_yml_is_actually_gated_and_the_reader_saw_it) runs in `python-tests`, which issue #459
# deliberately removed from every `needs:` list — so it gates NOTHING. This script is the half that
# runs in the GATE position (`ci-config-gate`). Without a floor here, a reader that parsed only 3 jobs
# would print `OK: 3 job(s); 0 fronted` and exit 0, and all 39 heavy legs would launch behind a
# vacuous gate while only the non-gating pytest went red. A gate whose own emptiness is a PASS is the
# exact vacuity class this file exists to prevent.
#
# Deliberately slack (the real figures are 24 / 21): these catch a COLLAPSE, not a job being added or
# removed. Raise them if the rollup ever shrinks below them legitimately.
MIN_JOBS = 20
MIN_FRONTED = 15


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="repository root (default: .)")
    ap.add_argument("--workflow", default=None,
                    help="the per-PR rollup workflow to check (TEST SEAM; rules 3/5 and the sanity "
                         "floors are specific to this repo's ci.yml, so pointing this at another "
                         "workflow yields confident nonsense). Default: "
                         "<repo-root>/.github/workflows/ci.yml")
    args = ap.parse_args(argv)

    root = Path(args.repo_root)
    if args.workflow is None and not root.is_dir():
        print(f"[{TAG}] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2
    path = Path(args.workflow) if args.workflow else root / ".github" / "workflows" / "ci.yml"
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
        for err in errors:
            print(f"[{TAG}] FINDING: {err}", file=sys.stderr)
        print(f"[{TAG}] FAIL: {len(errors)} gating-topology violation(s) in {path}", file=sys.stderr)
        return 1
    declared = set(GATE_JOBS) | set(NON_GATING_JOBS)
    gated = sum(1 for j, n in jobs.items() if (j not in declared) and n)
    # The floors apply to the REAL workflow only; the synthetic fixtures the tests pass via --workflow
    # are legitimately tiny.
    if args.workflow is None and (len(jobs) < MIN_JOBS or gated < MIN_FRONTED):
        print(f"[{TAG}] ERROR: {path}: parsed only {len(jobs)} job(s) and {gated} fronted, below the "
              f"sanity floor ({MIN_JOBS} / {MIN_FRONTED}). The topology cannot have shrunk this far, "
              f"so the READER is broken — refusing to report a vacuous OK. See MIN_JOBS in "
              f"tools/check_ci_gating.py.", file=sys.stderr)
        return 2
    print(f"[{TAG}] OK: {len(jobs)} job(s); {gated} fronted by {list(GATE_JOBS)}; "
          f"{list(NON_GATING_JOBS)} gate nothing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
