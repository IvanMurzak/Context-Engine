#!/usr/bin/env python3
"""CI fleet-manifest validator (R-QA-012) — the "CI consumes it" tie.

The manifest (docs/ci-fleet-manifest.json) maps every CI-enforced requirement to the named runner
class that enforces it, its tier, and its red-X policy. This gate keeps the manifest honest and in
sync with the live workflow; it runs in the ci-config-gate CI job every PR (it moved out of
python-tests in issue #459 — ci-config-gate is one of the two DETERMINISTIC gates that front the
rollup, so this validator must stay stdlib-only and free of network/subprocess/timing dependence).

Checks:
  1. Structural: manifest_version, runner_classes, gates present and well-typed.
  2. Every gate references a DECLARED runner class.
  3. Every red_x_policy is in the taxonomy; every tier is per-PR / nightly.
  4. Advisory-until-provisioned: a gate whose runner class has provisioned=false MUST be advisory
     (R-QA-012 — never blocking, never silently green).
  5. Every quarantine-with-issue gate NAMES an issue.
  6. Every gate with a non-null ci_job_id maps to a REAL job in the workflow its TIER lives in
     (per-PR gates in ci.yml, nightly gates in bench-nightly.yml — the DEFAULT workflow set below,
     which is why no --ci-workflow flag is needed to get a correct answer).
  7. minspec_floors (R-QA-007 — the committed floor table lives HERE, M4 T7 issue #141): present,
     with a non-empty platforms table; every platform row names a non-empty reference_device,
     exactly one positive target (target_frame_rate_hz | target_tick_rate_hz), and a DECLARED
     runner class; the not_applicable scope notes (android, ios) stay stated.
  8. PROSE STATUS-CLAIM DRIFT (M9 x11): a `description` may not make a status claim the
     machine-readable truth contradicts. See the rule-8 block below for the three claim classes,
     what each is checked against, and what this rule deliberately cannot see.

Exit code 0 = manifest valid; 1 = violation(s); 2 = configuration error.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

from _ci_common import load_json_or_exit, strip_comments

RED_X_POLICIES = {"blocking", "advisory", "quarantine-with-issue"}
TIERS = {"per-PR", "nightly"}

# --- the DEFAULT workflow set, per tier -----------------------------------------------------------
# Rule 6 needs BOTH workflow files: the per-PR rollup lives in ci.yml and the nightly benchmark jobs
# in bench-nightly.yml. Defaulting to both is what makes a bare `check_fleet_manifest.py --repo-root .`
# correct, and it closes a measured friction: run with ONLY ci.yml, this gate reported SEVEN
# confident-looking FALSE violations (the seven nightly-tier gates, whose jobs are bench-100k-nightly
# and density-nightly). A gate that cries wolf gets ignored, which is its own silent-failure mode.
#
# A missing file in this DEFAULT set is a hard configuration error (exit 2), never a silent skip —
# otherwise "the default resolved nothing" would look exactly like "every job exists".
WORKFLOW_DEFAULTS = {
    "per-PR": ".github/workflows/ci.yml",
    "nightly": ".github/workflows/bench-nightly.yml",
}

# --- rule 8: the prose STATUS-CLAIM drift gate (M9 x11) ------------------------------------------
# WHY IT EXISTS. Rules 1-7 read the machine-readable FIELDS and never the `description` prose, so a
# status claim in that prose can go false and ride along GREEN indefinitely. MEASURED (2026-07-27):
# after x7 fixed #437 and e12c-1 landed, the `editor-shell-cef-smoke` row still asserted "macOS ctest
# registration DISABLED pending #437" — a FALSE status claim that survived a FULL TASK CYCLE inside a
# CI-validated file. This rule checks the narrow class that actually bit us, deliberately NOT prose
# quality in general.
#
# EVERY PATTERN IS AN ASSERTION SHAPE, NOT A KEYWORD, and that distinction is load-bearing: e12c-2's
# CORRECTED text says x7 "removed the two DISABLED properties it had shipped them with" — it mentions
# DISABLED while claiming the opposite, and must stay green. Each pattern was measured against all 94
# live rows before being enabled: A and C hit 0 rows, B hits 8, of which 4 name a declared runner
# class (all correctly unprovisioned) and 4 capture the bare words `class` / `runner` from "until the
# runner class is provisioned" — which is exactly why B fires only on a name that is DECLARED or that
# LOOKS like a runner-class id (contains a hyphen).
#
# WHAT IT CANNOT SEE, stated so a green run is not over-read: the SECOND half of the measured defect
# was prose repeating a root-cause reading x7 had already DISPROVED. No machine-readable field records
# "the reason we once believed", so that half is unreachable from here. This gate covers the STATUS
# half only.
#
# A — "this gate is parked on issue #N": then the row must RECORD that issue in its own `issue` field.
#     Pure internal consistency, no judgement about policy — you cannot claim to be waiting on an
#     issue you do not name. The measured defect fails here twice over: it said "pending #437" on a
#     row whose red_x_policy was `blocking` and whose `issue` was absent.
_CLAIM_PARKED_ON_ISSUE = re.compile(
    r"(?i)\b(pending|awaiting|blocked on|parked on)\s+(?:issue\s+)?#(\d+)")
# B — "advisory until <runner class> is provisioned": then that runner class must exist and must
#     actually be unprovisioned. This is the prose side of rule 4.
_CLAIM_AWAITING_PROVISION = re.compile(
    r"(?i)(?:until|when|once)\s+(?:the\s+)?[`'\"]?([a-z][a-z0-9-]{2,})[`'\"]?\s+"
    r"(?:runner\s+class\s+)?(?:is|are|gets?)\s+provisioned")
# C — "the registration is DISABLED": then some test in the tree must really carry a DISABLED
#     property, and when the claim names a literally-registered ctest, that test must be the one.
_CLAIM_DISABLED = re.compile(
    r"(?i)\b(?:is|are|stays?|remains?|ships?|left|marked|currently|registration|registered)"
    r"\s+(?:\w+\s+){0,3}DISABLED\b"
    r"|\bDISABLED\s+(?:pending|awaiting|because|until)\b"
    r"|\bregistration\s+DISABLED\b")

_ISSUE_NUMBER = re.compile(r"(\d+)")
_CTEST_TOKEN = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{2,}")

# --- reading the ctest registrations rule 8's claim C is checked against --------------------------
_ADD_TEST_NAME = re.compile(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_.:${}-]+)")
_SET_TESTS_PROPERTIES = re.compile(r"\bset_tests_properties\s*\(([^)]*)\)", re.DOTALL)
_DISABLED_PROPERTY = re.compile(r"\bDISABLED\b\s+(?:TRUE|ON|1)\b", re.IGNORECASE)
# Generated CMake binary trees are pruned exactly as tools/check_cef_staging.py prunes them; its
# `_PRUNED_DIRS` comment is the canonical statement of why the repo's REAL src/editor/build/ module
# forbids keying the prune off the name "build" (a binary tree is identified by its CMakeCache.txt).
_PRUNED_CMAKE_DIRS = frozenset({"_deps", "_cef", "CMakeFiles", "node_modules"})


def ctest_registrations(src: Path) -> tuple[set[str], set[str]]:
    """(every LITERAL `add_test(NAME ...)` under `src`, every test carrying a DISABLED property).

    LIMITATION, stated because it bounds rule 8's claim C: a test registered through a variable —
    `add_test(NAME ${SMOKE_TEST} ...)`, which is how all ten live CEF smokes are registered — is
    invisible to a source scan. That is the same `${var}` blindness tools/check_cef_staging.py's
    module docstring records as having hidden the whole macOS half of its own tree. So a claim that
    NAMES a test is cross-checked only when that name is literally registered; otherwise the check
    falls back to "some test in this tree must really be disabled", which is precisely what made the
    measured claim false — x7 removed the last two DISABLED properties, leaving zero.

    COMMENTS ARE STRIPPED FIRST, and that is not cosmetic: this repository documents its `DISABLED
    TRUE` history IN CMake comments (src/editor/shell/cef/CMakeLists.txt records e12c-1's two disabled
    macOS smokes in prose, and a comment there also spells `add_test(COMMAND ...)`). Since
    `_SET_TESTS_PROPERTIES` scans to the next `)`, a comment sitting inside a real
    `set_tests_properties(...)` call would otherwise put a live test into `disabled` — and a non-empty
    `disabled` is exactly what silences claim C's tree-wide branch, turning rule 8 quiet rather than
    wrong. Reading prose as CMake is the same class of defect rule 8 exists to catch.
    """
    names: set[str] = set()
    disabled: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(src):
        directory = Path(dirpath)
        dirnames[:] = [
            d
            for d in dirnames
            if d not in _PRUNED_CMAKE_DIRS
            and not d.startswith(".")
            and not (directory / d / "CMakeCache.txt").is_file()
        ]
        if "CMakeLists.txt" not in filenames:
            continue
        text = strip_comments(
            (directory / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace"))
        names.update(n for n in _ADD_TEST_NAME.findall(text) if "${" not in n)
        for match in _SET_TESTS_PROPERTIES.finditer(text):
            body = match.group(1)
            head = body.split("PROPERTIES")[0]
            if _DISABLED_PROPERTY.search(body):
                disabled.update(t for t in _CTEST_TOKEN.findall(head) if "${" not in t)
    return names, disabled


def status_claim_errors(
    gate: dict,
    runner_classes: dict,
    registrations: tuple[set[str], set[str]] | None,
) -> list[str]:
    """Rule 8 for ONE gate row: the status claims its prose makes, checked against the truth."""
    gid = gate.get("id", "<no-id>")
    description = gate.get("description")
    if not isinstance(description, str) or not description:
        return []
    errors: list[str] = []

    # A — parked on an issue the row does not record.
    recorded = _ISSUE_NUMBER.findall(str(gate.get("issue") or ""))
    for match in _CLAIM_PARKED_ON_ISSUE.finditer(description):
        number = match.group(2)
        if number not in recorded:
            errors.append(
                f"gate {gid!r}: description claims {match.group(0)!r} but the row's 'issue' field "
                f"({gate.get('issue')!r}) does not name #{number} — a gate that says it is waiting "
                f"on an issue must record that issue (rule 8, status-claim drift)")

    # B — awaiting the provisioning of a runner class that is already provisioned, or not declared.
    for match in _CLAIM_AWAITING_PROVISION.finditer(description):
        name = match.group(1)
        rc = runner_classes.get(name)
        if rc is None:
            if "-" in name:
                errors.append(
                    f"gate {gid!r}: description claims {match.group(0)!r} but {name!r} is not "
                    f"declared in runner_classes (rule 8, status-claim drift)")
            continue
        if isinstance(rc, dict) and rc.get("provisioned") is not False:
            errors.append(
                f"gate {gid!r}: description claims {match.group(0)!r} but runner_class {name!r} is "
                f"already declared provisioned={rc.get('provisioned')!r} (rule 8, status-claim "
                f"drift — this is the prose side of rule 4)")

    # C — claiming a DISABLED registration the sources do not carry.
    if registrations is not None:
        registered, disabled = registrations
        for match in _CLAIM_DISABLED.finditer(description):
            sentence = _claim_sentence(description, match.start())
            named = {t for t in _CTEST_TOKEN.findall(sentence) if t in registered}
            if named:
                if not (named & disabled):
                    errors.append(
                        f"gate {gid!r}: description claims {match.group(0).strip()!r} of "
                        f"{sorted(named)}, but no such ctest carries a DISABLED property in the "
                        f"CMake sources (rule 8, status-claim drift)")
            elif not disabled:
                errors.append(
                    f"gate {gid!r}: description claims {match.group(0).strip()!r}, but NO ctest in "
                    f"the tree carries a DISABLED property — the claim cannot be true (rule 8, "
                    f"status-claim drift). This is the exact shape that survived a full task cycle "
                    f"on the editor-shell-cef-smoke row after x7 removed the last two")
    return errors


def _claim_sentence(text: str, position: int) -> str:
    """The sentence containing `position` — the scope a claim's ctest names are read from."""
    start = max(text.rfind(". ", 0, position), text.rfind("; ", 0, position)) + 1
    end = min(
        (i for i in (text.find(". ", position), text.find("; ", position)) if i != -1),
        default=len(text),
    )
    return text[start:end]


def validate(
    manifest: dict,
    workflow_text: str | None = None,
    *,
    workflows: dict[str, str] | None = None,
    registrations: tuple[set[str], set[str]] | None = None,
) -> list[str]:
    """Return a list of human-readable violations (empty == valid).

    `workflows` maps a TIER to the text of the workflow file that tier's jobs must live in, which is
    what makes rule 6 tier-aware; `workflow_text` is the legacy tier-BLIND form (a job may live
    anywhere in the one concatenated text), retained for DIRECT callers such as
    tools/tests/test_check_fleet_manifest.py. `main()` no longer produces it at all — it always passes
    `workflows`, and an explicit --ci-workflow file SUPPLEMENTS the tier it is attributed to rather
    than every tier at once, so the tier↔workflow split stays asserted under the invocation shapes the
    docs advertise. A file whose basename matches no default is the ONE remaining tier-blind case, by
    construction: naming an unknown workflow says nothing about which tier owns its jobs.
    `registrations` is `ctest_registrations()`'s pair, required by rule 8's claim C.
    """
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

        # Rule 6: a claimed CI job must exist in the live workflow — in the one its TIER lives in,
        # when the caller supplied the per-tier mapping (the default set). A nightly gate's job is
        # NOT in ci.yml and must not be looked for there: reading the two files as one pool is what
        # let the tier↔workflow split go unasserted.
        job = gate.get("ci_job_id")
        haystack = None
        scope = "the workflow"
        if job and workflows:
            haystack = workflows.get(gate.get("tier"))
            if haystack is None:
                haystack = "\n".join(workflows.values())
            else:
                scope = f"the {gate.get('tier')} workflow"
        elif job:
            haystack = workflow_text
        if job and haystack is not None:
            if not re.search(rf"(?m)^\s{{0,4}}{re.escape(job)}:\s*$", haystack):
                errors.append(
                    f"gate {gid!r}: ci_job_id {job!r} has no matching job in {scope}")

        # Rule 8: the prose may not contradict the machine-readable truth.
        errors.extend(status_claim_errors(gate, runner_classes, registrations))

    # Rule 7: the R-QA-007 min-spec floor table (committed HERE per R-QA-007/R-QA-012).
    floors = manifest.get("minspec_floors")
    if not isinstance(floors, dict):
        errors.append("missing minspec_floors — the R-QA-007 floor table lives in this manifest")
    else:
        platforms = floors.get("platforms")
        if not isinstance(platforms, dict) or not platforms:
            errors.append("minspec_floors.platforms must be a non-empty object")
            platforms = {}
        for name, row in platforms.items():
            if not isinstance(row, dict):
                errors.append(f"minspec_floors platform {name!r}: must be an object")
                continue
            device = row.get("reference_device")
            if not isinstance(device, str) or not device.strip():
                errors.append(
                    f"minspec_floors platform {name!r}: missing non-empty 'reference_device'")
            targets = [k for k in ("target_frame_rate_hz", "target_tick_rate_hz") if k in row]
            if len(targets) != 1:
                errors.append(
                    f"minspec_floors platform {name!r}: exactly ONE of target_frame_rate_hz / "
                    f"target_tick_rate_hz required (found {len(targets)})")
            else:
                value = row[targets[0]]
                if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
                    errors.append(
                        f"minspec_floors platform {name!r}: {targets[0]} must be a positive "
                        f"number (was {value!r})")
            rc_name = row.get("runner_class")
            if rc_name not in runner_classes:
                errors.append(
                    f"minspec_floors platform {name!r}: unknown runner_class {rc_name!r}")
        not_applicable = floors.get("not_applicable")
        if not isinstance(not_applicable, dict) or not {"android", "ios"} <= set(not_applicable):
            errors.append(
                "minspec_floors.not_applicable must state the android + ios scope notes "
                "(R-QA-007 platform scope honesty)")

    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # --repo-root is the shape every other gate in tools/ accepts (check_cef_staging.py,
    # check_config_writers.py, check_session_ownership.py, ...). This gate used to reject it, which
    # made it the one gate a caller had to remember was different.
    ap.add_argument("--repo-root", default=".", help="the Context-Engine repository root")
    ap.add_argument("--manifest", default=None,
                    help="path to the CI fleet manifest JSON "
                         "(default: <repo-root>/docs/ci-fleet-manifest.json)")
    ap.add_argument("--ci-workflow", action="append", default=None,
                    help="optional, repeatable: cross-check ci_job_id values against these workflow "
                         "YAMLs. An explicit list is tier-BLIND (a job may live in any of them); the "
                         "DEFAULT set is tier-AWARE, because it knows which file carries which tier")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        print(f"[fleet-manifest] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2

    # A RELATIVE --manifest resolves against --repo-root, like every other input here; an absolute one
    # is honoured as given. Reading it from the CWD instead would validate one repo's manifest against
    # another repo's workflows and src/ tree, and report the mismatch as manifest drift.
    manifest_path = root / args.manifest if args.manifest else root / "docs/ci-fleet-manifest.json"
    manifest = load_json_or_exit(manifest_path, tag="fleet-manifest")

    # The DEFAULT set is ALWAYS loaded, and an explicit --ci-workflow list SUPPLEMENTS it rather than
    # narrowing it. That is what makes "considered only ci.yml" stop reporting false violations under
    # every invocation shape, not just the bare one: the second workflow file is resolved by the gate
    # itself, so a caller who names one file (the shape the old docs showed) is no longer told its
    # manifest has seven violations it does not have.
    workflows: dict[str, str] = {}
    for tier, relative in WORKFLOW_DEFAULTS.items():
        try:
            workflows[tier] = (root / relative).read_text(encoding="utf-8")
        except OSError as exc:
            # NOT a silent skip: a default that resolved nothing would look exactly like a manifest
            # whose every ci_job_id exists.
            print(f"[fleet-manifest] ERROR: cannot read the default {tier} workflow "
                  f"{relative}: {exc}", file=sys.stderr)
            return 2
    for wf in args.ci_workflow or []:
        try:
            extra = Path(wf).read_text(encoding="utf-8")
        except OSError as exc:
            print(f"[fleet-manifest] ERROR: cannot read workflow {wf}: {exc}", file=sys.stderr)
            return 2
        # ATTRIBUTE the extra to ONE tier when its basename is one of the defaults. Appending every
        # extra to every tier instead re-opens the pool rule 6 was tightened to close — MEASURED: with
        # `--ci-workflow .github/workflows/bench-nightly.yml`, a per-PR row pointing at the
        # nightly-only `bench-100k-nightly` job passed, while the bare default set correctly reported
        # it. An UNRECOGNISED file keeps the tier-blind escape hatch (it goes to every tier), because
        # a caller naming some third workflow has told us nothing about which tier owns it.
        owner = next((tier for tier, relative in WORKFLOW_DEFAULTS.items()
                      if Path(relative).name == Path(wf).name), None)
        for tier in (tuple(workflows) if owner is None else (owner,)):
            workflows[tier] = f"{workflows[tier]}\n{extra}"
    # Never the tier-BLIND form from here: `workflows` above always carries the per-tier mapping, so
    # rule 6 stays tier-aware even when --ci-workflow supplemented it. Only direct callers pass it.
    workflow_text = None

    src = root / "src"
    if not src.is_dir():
        print(f"[fleet-manifest] ERROR: no src/ directory under {root} (bad --repo-root?) — rule 8 "
              f"cannot read the ctest registrations it checks DISABLED claims against",
              file=sys.stderr)
        return 2
    registrations = ctest_registrations(src)

    errors = validate(manifest, workflow_text, workflows=workflows, registrations=registrations)
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
