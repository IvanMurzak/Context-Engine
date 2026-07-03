#!/usr/bin/env python3
"""CI fleet-manifest validator (R-QA-012) — the "CI consumes it" tie.

The manifest (docs/ci-fleet-manifest.json) maps every CI-enforced requirement to the named runner
class that enforces it, its tier, and its red-X policy. This gate keeps the manifest honest and in
sync with the live workflow; it runs in the python-tests CI job every PR.

Checks:
  1. Structural: manifest_version, runner_classes, gates present and well-typed.
  2. Every gate references a DECLARED runner class.
  3. Every red_x_policy is in the taxonomy; every tier is per-PR / nightly.
  4. Advisory-until-provisioned: a gate whose runner class has provisioned=false MUST be advisory
     (R-QA-012 — never blocking, never silently green).
  5. Every quarantine-with-issue gate NAMES an issue.
  6. (with --ci-workflow) every gate with a non-null ci_job_id maps to a REAL job in the workflow.

Exit code 0 = manifest valid; 1 = violation(s); 2 = configuration error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from _ci_common import load_json_or_exit

RED_X_POLICIES = {"blocking", "advisory", "quarantine-with-issue"}
TIERS = {"per-PR", "nightly"}


def validate(manifest: dict, workflow_text: str | None) -> list[str]:
    """Return a list of human-readable violations (empty == valid)."""
    errors: list[str] = []

    if manifest.get("manifest_version") is None:
        errors.append("missing manifest_version")

    runner_classes = manifest.get("runner_classes")
    if not isinstance(runner_classes, dict) or not runner_classes:
        errors.append("runner_classes must be a non-empty object")
        runner_classes = {}

    gates = manifest.get("gates")
    if not isinstance(gates, list) or not gates:
        errors.append("gates must be a non-empty array")
        gates = []

    # Runner-class shape.
    for name, rc in runner_classes.items():
        if not isinstance(rc, dict):
            errors.append(f"runner_class {name!r}: must be an object")
            continue
        if not isinstance(rc.get("provisioned"), bool):
            errors.append(f"runner_class {name!r}: 'provisioned' must be a bool")
        if not rc.get("isolation"):
            errors.append(f"runner_class {name!r}: missing 'isolation'")

    seen_ids: set[str] = set()
    for gate in gates:
        if not isinstance(gate, dict):
            errors.append(f"gate {gate!r}: must be an object")
            continue
        gid = gate.get("id", "<no-id>")
        if gid in seen_ids:
            errors.append(f"gate {gid!r}: duplicate id")
        seen_ids.add(gid)

        policy = gate.get("red_x_policy")
        if policy not in RED_X_POLICIES:
            errors.append(f"gate {gid!r}: red_x_policy {policy!r} not in {sorted(RED_X_POLICIES)}")
        if gate.get("tier") not in TIERS:
            errors.append(f"gate {gid!r}: tier {gate.get('tier')!r} not in {sorted(TIERS)}")

        rc_name = gate.get("runner_class")
        rc = runner_classes.get(rc_name)
        if rc is None:
            errors.append(f"gate {gid!r}: unknown runner_class {rc_name!r}")
        elif isinstance(rc, dict) and rc.get("provisioned") is False and policy != "advisory":
            # Rule 4: an unprovisioned runner class can only back an advisory gate.
            errors.append(
                f"gate {gid!r}: runner_class {rc_name!r} is not provisioned, so red_x_policy must be "
                f"'advisory' (was {policy!r}) — R-QA-012 advisory-until-provisioned")

        if policy == "quarantine-with-issue" and not gate.get("issue"):
            errors.append(f"gate {gid!r}: quarantine-with-issue requires a non-empty 'issue'")

        # Rule 6: a claimed CI job must exist in the live workflow.
        job = gate.get("ci_job_id")
        if job and workflow_text is not None:
            if not re.search(rf"(?m)^\s{{0,4}}{re.escape(job)}:\s*$", workflow_text):
                errors.append(
                    f"gate {gid!r}: ci_job_id {job!r} has no matching job in the workflow")

    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest", default="docs/ci-fleet-manifest.json",
                    help="path to the CI fleet manifest JSON")
    ap.add_argument("--ci-workflow", default=None,
                    help="optional: cross-check ci_job_id values against this workflow YAML")
    args = ap.parse_args(argv)

    manifest = load_json_or_exit(Path(args.manifest), tag="fleet-manifest")

    workflow_text: str | None = None
    if args.ci_workflow:
        try:
            workflow_text = Path(args.ci_workflow).read_text(encoding="utf-8")
        except OSError as exc:
            print(f"[fleet-manifest] ERROR: cannot read workflow {args.ci_workflow}: {exc}",
                  file=sys.stderr)
            return 2

    errors = validate(manifest, workflow_text)
    if errors:
        print(f"[fleet-manifest] {len(errors)} violation(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    gates = manifest.get("gates", [])
    advisory = sum(1 for g in gates if g.get("red_x_policy") == "advisory")
    print(f"[fleet-manifest] OK: {len(gates)} gates, "
          f"{len(manifest.get('runner_classes', {}))} runner classes, {advisory} advisory.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
