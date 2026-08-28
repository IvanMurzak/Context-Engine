#!/usr/bin/env python
# Run-level worktree teardown for `isolation: run` pipelines (frozen contract:
# the pipeline plugin's docs/worktree-hook-contract.md). Policy: PRESERVE the
# slot on halted/depth-exhausted (post-mortem + same-worktree resume), reap it on
# completed and create-failed. Honors PIPELINE_WT_DELETE_BRANCHES within the
# machine-owned `worktree-*` branch namespace only. Prints exactly one JSON
# object on stdout; soft failures report {"ok": false, "detail": ...} with exit 0
# so a failed teardown never strands the run.
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile


def log(msg):
    print(f"worktree-destroy: {msg}", file=sys.stderr)


def emit(obj):
    json.dump(obj, sys.stdout)
    print()
    sys.exit(0)


def run_git(args, cwd=None, timeout=300):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, timeout=timeout)


def posix(p):
    return os.path.abspath(p).replace("\\", "/")


def norm(p):
    r = posix(p).rstrip("/")
    return r.lower() if os.name == "nt" else r


def is_under(p, d):
    a, b = norm(p), norm(d)
    return a == b or a.startswith(b + "/")


def slot_root_base():
    override = os.environ.get("PIPELINE_WT_ROOT", "").strip()
    if override:
        return posix(override)
    if os.name == "nt" and os.path.isdir("C:/tmp"):
        return "C:/tmp/pipeline-worktrees"
    return posix(os.path.join(tempfile.gettempdir(), "pipeline-worktrees"))


def slot_dir(repo, name):
    # Must stay identical to worktree-create.py's layout: it is the fallback for
    # a create-failed teardown where PIPELINE_WT_WORKTREE_PATH was never reported.
    digest = hashlib.sha256(norm(repo).encode("utf-8")).hexdigest()[:8]
    label = re.sub(r"[^A-Za-z0-9._-]", "-", os.path.basename(norm(repo)))[:24] or "project"
    return f"{slot_root_base()}/{label}-{digest}/{name}"


def too_shallow(p):
    body = re.sub(r"^[A-Za-z]:/", "", posix(p)).lstrip("/")
    return len([s for s in body.split("/") if s]) < 2


def registered_paths(repo):
    r = run_git(["worktree", "list", "--porcelain"], cwd=repo)
    out = set()
    for line in r.stdout.splitlines():
        if line.startswith("worktree "):
            out.add(norm(line[len("worktree "):].strip()))
    return out


def rmtree_force(path):
    def onerr(fn, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        fn(p)
    shutil.rmtree(path, onerror=onerr)


def main():
    name = os.environ.get("PIPELINE_WT_NAME", "").strip()
    if not name:
        emit({"ok": False, "detail": "PIPELINE_WT_NAME is not set"})
    outcome = os.environ.get("PIPELINE_WT_OUTCOME", "").strip() or "unknown"
    project_root = os.environ.get("PIPELINE_WT_PROJECT_ROOT", "").strip() or os.getcwd()

    if outcome not in ("completed", "create-failed"):
        # halted / depth-exhausted / anything unknown: keep the evidence.
        emit({"ok": True, "detail": f"worktree preserved for post-mortem/resume (outcome={outcome})"})

    top = run_git(["rev-parse", "--show-toplevel"], cwd=project_root)
    if top.returncode != 0 or not top.stdout.strip():
        emit({"ok": False, "detail": f"not a git repository: {project_root}"})
    repo = posix(top.stdout.strip())

    slot = os.environ.get("PIPELINE_WT_WORKTREE_PATH", "").strip() or slot_dir(repo, name)
    slot = posix(slot)
    branch = f"worktree-{name}"
    env_file = slot + ".env"
    delete_branches = os.environ.get("PIPELINE_WT_DELETE_BRANCHES", "0").strip() == "1"

    if os.environ.get("PIPELINE_WT_DRY_RUN", "0").strip() == "1":
        emit({"ok": True, "detail": f"dry-run: would reap {slot} (branch {branch}, "
                                    f"delete_branches={delete_branches})"})

    # Refusals before anything is deleted — a bad record must never cost the
    # user's checkout or the running process's own cwd.
    if is_under(slot, repo):
        emit({"ok": False, "detail": f"refusing: slot {slot} is inside the repository {repo}"})
    if is_under(os.getcwd(), slot):
        emit({"ok": False, "detail": f"refusing: the current working directory is inside {slot}"})
    if too_shallow(slot):
        emit({"ok": False, "detail": f"refusing: {slot} is too close to a filesystem root to be a slot"})

    problems = []
    run_git(["worktree", "prune"], cwd=repo)
    if norm(slot) in registered_paths(repo):
        rm = run_git(["worktree", "remove", "--force", slot], cwd=repo, timeout=300)
        if rm.returncode != 0:
            problems.append(f"git worktree remove failed (exit {rm.returncode}): "
                            f"{(rm.stderr or rm.stdout).strip()}")
    if not problems and os.path.isdir(slot):
        try:
            rmtree_force(slot)
        except OSError as e:
            problems.append(f"could not delete {slot}: {e}")

    if delete_branches and not problems:
        # -D, not -d: a squash-merged run branch reads as unmerged to git forever.
        # Blast radius bounded to the worktree-* namespace this hook creates in.
        if run_git(["rev-parse", "--verify", "--quiet", f"refs/heads/{branch}"], cwd=repo).returncode == 0:
            d = run_git(["branch", "-D", branch], cwd=repo)
            if d.returncode != 0:
                problems.append(f"git branch -D {branch} failed: {(d.stderr or d.stdout).strip()}")

    if not problems and os.path.isfile(env_file) and not is_under(env_file, repo):
        try:
            os.unlink(env_file)
        except OSError as e:
            problems.append(f"could not delete {env_file}: {e}")

    run_git(["worktree", "prune"], cwd=repo)
    if problems:
        emit({"ok": False, "detail": "; ".join(problems)})
    emit({"ok": True})


if __name__ == "__main__":
    main()
