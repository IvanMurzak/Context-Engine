#!/usr/bin/env python
# Run-level worktree provisioner for `isolation: run` pipelines (frozen contract:
# the pipeline plugin's docs/worktree-hook-contract.md). Provisions ONE plain git
# worktree of this repository OUTSIDE the checkout, cut from origin/<base>, plus
# a dotenv slot description. Idempotent per PIPELINE_WT_NAME. Prints exactly one
# JSON object on stdout; all diagnostics go to stderr; non-zero exit = failed
# provision (the CLI then halts the run and calls worktree-destroy once with
# PIPELINE_WT_OUTCOME=create-failed).
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

# The env file is read with `set -a && source`, so every value must be safe
# UNQUOTED. Allow-list mirrors the pipeline CLI's own provisioner.
ENV_VALUE_RE = re.compile(r"^[A-Za-z0-9._:/+@,~=-]*$")
TILDE_EXPANDS_RE = re.compile(r"(^~|:~)")


def log(msg):
    print(f"worktree-create: {msg}", file=sys.stderr)


def fail(msg):
    log(msg)
    sys.exit(1)


def run_git(args, cwd=None, timeout=120):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, timeout=timeout)


def posix(p):
    return os.path.abspath(p).replace("\\", "/")


def norm(p):
    r = posix(p).rstrip("/")
    return r.lower() if os.name == "nt" else r


def slot_root_base():
    override = os.environ.get("PIPELINE_WT_ROOT", "").strip()
    if override:
        return posix(override)
    if os.name == "nt" and os.path.isdir("C:/tmp"):
        return "C:/tmp/pipeline-worktrees"
    return posix(os.path.join(tempfile.gettempdir(), "pipeline-worktrees"))


def slot_dir(repo, name):
    digest = hashlib.sha256(norm(repo).encode("utf-8")).hexdigest()[:8]
    label = re.sub(r"[^A-Za-z0-9._-]", "-", os.path.basename(norm(repo)))[:24] or "project"
    return f"{slot_root_base()}/{label}-{digest}/{name}"


def ref_exists(repo, ref):
    return run_git(["rev-parse", "--verify", "--quiet", ref], cwd=repo).returncode == 0


def registered_paths(repo):
    r = run_git(["worktree", "list", "--porcelain"], cwd=repo)
    out = set()
    for line in r.stdout.splitlines():
        if line.startswith("worktree "):
            out.add(norm(line[len("worktree "):].strip()))
    return out


def check_env_entry(key, value):
    if not ENV_VALUE_RE.match(value) or TILDE_EXPANDS_RE.search(value):
        fail(f"env value for {key} ({value!r}) cannot be written unquoted; "
             f"set PIPELINE_WT_ROOT to a path without spaces or shell metacharacters")


def main():
    name = os.environ.get("PIPELINE_WT_NAME", "").strip()
    if not name:
        fail("PIPELINE_WT_NAME is not set")
    if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]*$", name):
        fail(f"unsafe slot name {name!r}")
    base = os.environ.get("PIPELINE_WT_BASE_BRANCH", "").strip() or "main"
    project_root = os.environ.get("PIPELINE_WT_PROJECT_ROOT", "").strip() or os.getcwd()
    submodules = [s for s in os.environ.get("PIPELINE_WT_SUBMODULES", "").split(",") if s.strip()]
    if submodules:
        fail("this hook provisions a plain Context-Engine worktree and supports no submodules")

    top = run_git(["rev-parse", "--show-toplevel"], cwd=project_root)
    if top.returncode != 0 or not top.stdout.strip():
        fail(f"not a git repository: {project_root}")
    repo = posix(top.stdout.strip())

    slot = slot_dir(repo, name)
    branch = f"worktree-{name}"
    env_file = slot + ".env"

    if os.environ.get("PIPELINE_WT_DRY_RUN", "0").strip() == "1":
        json.dump({"worktree_path": slot, "branch": branch, "env_file": env_file,
                   "port_base": 0, "ports": {}}, sys.stdout)
        print()
        return

    run_git(["worktree", "prune"], cwd=repo)
    reused = norm(slot) in registered_paths(repo) and os.path.isdir(slot)
    if reused:
        log(f"reusing existing slot {slot}")
    else:
        try:
            fetched = run_git(["fetch", "--quiet", "origin", base], cwd=repo, timeout=90)
            if fetched.returncode != 0:
                log(f"fetch of origin/{base} failed ({fetched.stderr.strip() or fetched.returncode}); "
                    f"continuing with local refs")
        except subprocess.TimeoutExpired:
            log("fetch timed out; continuing with local refs")
        os.makedirs(os.path.dirname(slot), exist_ok=True)
        if ref_exists(repo, f"refs/heads/{branch}"):
            # Resume after a crash: the branch survived, the worktree did not.
            add = run_git(["worktree", "add", slot, branch], cwd=repo, timeout=600)
        else:
            if ref_exists(repo, f"refs/remotes/origin/{base}"):
                start = f"origin/{base}"
            elif ref_exists(repo, f"refs/heads/{base}"):
                start = base
            else:
                fail(f"base branch '{base}' not found locally or on origin")
            add = run_git(["worktree", "add", "-b", branch, slot, start], cwd=repo, timeout=600)
        if add.returncode != 0:
            fail(f"git worktree add failed (exit {add.returncode}): "
                 f"{(add.stderr or add.stdout).strip()}")

    # The slot must BE its own working tree — a redirect into another checkout
    # would make a worker commit into the user's real tree believing it isolated.
    t = run_git(["rev-parse", "--show-toplevel"], cwd=slot)
    if t.returncode != 0 or norm(t.stdout.strip()) != norm(slot):
        fail(f"refusing slot {slot}: it resolves to {t.stdout.strip() or '<nothing>'} instead of itself")

    values = [
        ("RUN_ID", os.environ.get("PIPELINE_WT_RUN_ID", "").strip() or name),
        ("WORKTREE_NAME", name),
        ("WORKTREE_PATH", slot),
        ("WORKTREE_BRANCH", branch),
        ("PROJECT_ROOT", repo),
        ("BASE_BRANCH", base),
    ]
    for k, v in values:
        check_env_entry(k, v)
    body = "# generated by .pipeline/.hooks/worktree-create.py — do not edit by hand\n"
    body += "".join(f"{k}={v}\n" for k, v in values)
    with open(env_file, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)

    json.dump({"worktree_path": slot, "branch": branch, "env_file": env_file,
               "port_base": 0, "ports": {}}, sys.stdout)
    print()


if __name__ == "__main__":
    main()
