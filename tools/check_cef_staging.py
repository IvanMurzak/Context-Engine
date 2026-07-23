#!/usr/bin/env python3
"""Fail on any build-graph shape that lets two CEF payload copies race into one directory (#360).

The CEF distribution ships a `COPY_FILES()` macro that attaches the binary + resource copy as a
POST_BUILD custom command on ONE target. That is correct where a directory holds a single CEF
executable, and wrong the moment several executables share a staging destination: ninja links them in
parallel, so two post-build steps run `cmake -E copy_directory` over `Resources/locales` (or
`copy_if_different` over `chrome_elf.dll`) simultaneously. POSIX tolerates replacing a file another
process holds open; Windows returns a sharing violation and the build dies with

    Error copying directory from ".../_cef/<triple>/Resources/locales"
                              to ".../build/dev/editor/shell/Release/locales".

That is issue #360 — intermittent purely because it depends on ninja's scheduling. It cost repeated
`editor-cef-smoke (windows-latest)` rerun cycles and sent one investigation after phantom orphaned
processes. The fix was structural: ONE staging target, and every consumer takes an ordinary
`add_dependencies()` edge on it. This lint is what keeps it that way.

WHAT IT CHECKS (over the CMake sources, so it runs on every default `build` leg with no CEF at all —
the same tier as tools/check_no_raw_key_handlers.py / tools/check_webui_assets.py):

  1. SINGLE WRITER. No CEF staging destination has more than one writer. A destination is identified
     by the directory that calls `context_acquire_cef()` — that is where `SET_CEF_TARGET_OUT_DIR()`
     runs, so `${CEF_TARGET_OUT_DIR}` in that directory AND in every subdirectory added beneath it
     resolves to the same path. Writers are the targets passed to `COPY_FILES(... CEF_TARGET_OUT_DIR)`
     plus any `context_cef_stage_payload()` stage target rooted there.

  2. COMPLETE CONSUMERS. Where a destination is served by a stage target, every CEF executable rooted
     there (a target passed to `SET_EXECUTABLE_TARGET_PROPERTIES()`) declares
     `add_dependencies(<exe> ... <stage target> ...)`. Without that edge an executable can be linked
     and run before its payload is staged — the failure mode a new smoke exe silently introduces.

  3. NO MASKING, AND STILL INCREMENTAL. The staging implementation in cmake/ContextCef.cmake contains
     no retry loop, sleep, timeout or ignored exit code (masking the race would leave a half-staged
     output directory that fails later and far more confusingly), and it is stamp-guarded — the
     OUTPUT form of `add_custom_command`, never POST_BUILD — so it re-runs when the pinned payload
     changes and not on every build.

Exit codes: 0 = clean, 1 = at least one finding, 2 = a configuration error.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# --- CMake call-site patterns ---------------------------------------------------------------------
# All are applied to COMMENT-STRIPPED text, so prose mentioning a macro never counts as a call.
_ACQUIRE = re.compile(r"\bcontext_acquire_cef\s*\(")
_STAGE_CALL = re.compile(r"\bcontext_cef_stage_payload\s*\(\s*([A-Za-z_]\w*)")
# COPY_FILES(<target> <files> <src dir> <dst dir>) — possibly wrapped across lines.
_COPY_FILES = re.compile(r"\bCOPY_FILES\s*\(\s*([A-Za-z_$][\w${}]*)([^)]*)\)", re.DOTALL)
# Only a LITERAL target name is checkable; a ${var} name is resolved at configure time and is left to
# the configure-time audit in src/editor/shell/CMakeLists.txt.
_CEF_EXE = re.compile(r"\bSET_EXECUTABLE_TARGET_PROPERTIES\s*\(\s*([A-Za-z_]\w*)\s*\)")
_ADD_DEPENDENCIES = re.compile(r"\badd_dependencies\s*\(\s*([A-Za-z_]\w*)([^)]*)\)", re.DOTALL)

_STAGE_FUNCTION = re.compile(
    r"\bfunction\s*\(\s*context_cef_stage_payload\b.*?\bendfunction\s*\(", re.DOTALL
)

# Constructs that would turn "remove the race" into "hide the race". `||` and `& exit 0` swallow a
# failed copy; sleep/retry/timeout re-run it instead of ordering it.
_MASKING = (
    (re.compile(r"\|\|"), "an `||` fallback that would swallow a failed copy"),
    (re.compile(r"\bsleep\b", re.IGNORECASE), "a sleep"),
    # No trailing \b: `retry_copy.cmake` / `retrying` are retries too.
    (re.compile(r"\bretr(?:y|ies|ying)", re.IGNORECASE), "a retry"),
    (re.compile(r"\btimeout\b", re.IGNORECASE), "a timeout"),
    (re.compile(r"\bexit\s+0\b", re.IGNORECASE), "a forced `exit 0`"),
    (re.compile(r"\bERROR_QUIET\b"), "ERROR_QUIET (a suppressed failure)"),
)

_STAGE_IMPL = "cmake/ContextCef.cmake"

# Directory names that are never SOURCE, pruned during the walk. Note the repo has a REAL
# `src/editor/build/` module, so pruning cannot key off the name "build" — a generated CMake binary
# tree is identified by the CMakeCache.txt at its root instead. `_deps`/`_cef` are pruned by name too
# because a configure populates them BEFORE writing the cache, and the fetched CEF distribution ships
# its own COPY_FILES() call sites (cefsimple/cefclient) that are none of this repo's business.
_PRUNED_DIRS = frozenset({"_deps", "_cef", "CMakeFiles", "node_modules"})


def strip_comments(text: str) -> str:
    """Remove CMake `#` comments, honouring double-quoted strings (which may contain `#`)."""
    out: list[str] = []
    for line in text.splitlines():
        in_quotes = False
        cut = len(line)
        i = 0
        while i < len(line):
            ch = line[i]
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_quotes = not in_quotes
            elif ch == "#" and not in_quotes:
                cut = i
                break
            i += 1
        out.append(line[:cut])
    return "\n".join(out)


def stage_root(directory: Path, acquire_dirs: set[Path]) -> Path | None:
    """The nearest ancestor-or-self directory that acquires CEF — i.e. what ${CEF_TARGET_OUT_DIR}
    resolves to for `directory`, since a subdirectory inherits the plain variable from its parent."""
    for candidate in (directory, *directory.parents):
        if candidate in acquire_dirs:
            return candidate
    return None


def source_cmakelists(src: Path) -> list[Path]:
    """Every AUTHORED CMakeLists.txt under src/, with generated CMake binary trees pruned."""
    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(src):
        directory = Path(dirpath)
        # Prune generated/vendored subtrees in place so the walk never descends into them.
        dirnames[:] = [
            d
            for d in dirnames
            if d not in _PRUNED_DIRS
            and not d.startswith(".")
            and not (directory / d / "CMakeCache.txt").is_file()
        ]
        if "CMakeLists.txt" in filenames:
            found.append(directory / "CMakeLists.txt")
    return sorted(found)


def scan(repo_root: Path) -> tuple[list[str], int]:
    """Return (findings, files_scanned). Raises SystemExit(2) on a configuration error."""
    src = repo_root / "src"
    if not src.is_dir():
        raise SystemExit(f"check_cef_staging: no src/ directory under {repo_root} (bad --repo-root?)")

    lists = source_cmakelists(src)
    if not lists:
        raise SystemExit(f"check_cef_staging: no CMakeLists.txt found under {src}")

    texts = {p.parent: strip_comments(p.read_text(encoding="utf-8", errors="replace")) for p in lists}
    acquire_dirs = {d for d, t in texts.items() if _ACQUIRE.search(t)}

    findings: list[str] = []

    # Per staging destination: its writers, its stage target, and its CEF executables.
    writers: dict[Path, dict[str, Path]] = {}
    stages: dict[Path, tuple[str, Path]] = {}
    exes: dict[Path, dict[str, Path]] = {}
    deps: dict[str, set[str]] = {}

    for directory, text in texts.items():
        root = stage_root(directory, acquire_dirs)

        for match in _COPY_FILES.finditer(text):
            if "CEF_TARGET_OUT_DIR" not in match.group(2):
                continue  # a copy to some other destination is none of this lint's business
            target = match.group(1)
            if root is None:
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"COPY_FILES({target} ... ${{CEF_TARGET_OUT_DIR}}) but no context_acquire_cef() "
                    f"in this directory or any ancestor -- the destination is undefined here."
                )
                continue
            writers.setdefault(root, {}).setdefault(target, directory)

        for match in _STAGE_CALL.finditer(text):
            if root is None:
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"context_cef_stage_payload({match.group(1)}) needs context_acquire_cef() in the "
                    f"same directory scope (it supplies CEF_TARGET_OUT_DIR and the payload lists)."
                )
                continue
            if root in stages:
                findings.append(
                    f"{root.relative_to(repo_root).as_posix()}: two stage targets "
                    f"({stages[root][0]}, {match.group(1)}) serve one CEF staging destination."
                )
                continue
            stages[root] = (match.group(1), directory)
            writers.setdefault(root, {}).setdefault(match.group(1), directory)

        if root is not None:
            for match in _CEF_EXE.finditer(text):
                exes.setdefault(root, {}).setdefault(match.group(1), directory)

        for match in _ADD_DEPENDENCIES.finditer(text):
            deps.setdefault(match.group(1), set()).update(match.group(2).split())

    # --- check 1: one writer per destination ------------------------------------------------------
    for root, by_target in sorted(writers.items()):
        if len(by_target) > 1:
            listed = ", ".join(
                f"{t} ({d.relative_to(repo_root).as_posix()})" for t, d in sorted(by_target.items())
            )
            findings.append(
                f"{root.relative_to(repo_root).as_posix()}: {len(by_target)} targets stage the CEF "
                f"payload into the SAME ${{CEF_TARGET_OUT_DIR}} -- {listed}. Concurrent POST_BUILD "
                f"copies into one directory are issue #360 (a Windows sharing violation whenever "
                f"ninja links two of them together). Stage once via context_cef_stage_payload() and "
                f"give every consumer add_dependencies(<exe> <stage target>)."
            )

    # --- check 2: every CEF executable depends on its destination's stage target -------------------
    for root, (stage_target, _stage_dir) in sorted(stages.items()):
        for exe, directory in sorted(exes.get(root, {}).items()):
            if exe == stage_target:
                continue
            if stage_target not in deps.get(exe, set()):
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: CEF executable "
                    f"{exe} runs from the staging destination served by {stage_target} but does not "
                    f"depend on it. Add add_dependencies({exe} {stage_target}) -- do NOT give it its "
                    f"own COPY_FILES() POST_BUILD copy (issue #360)."
                )

    # --- check 3: the staging implementation neither masks failures nor re-copies every build ------
    impl_path = repo_root / _STAGE_IMPL
    if stages:
        if not impl_path.is_file():
            findings.append(
                f"{_STAGE_IMPL} is missing, but context_cef_stage_payload() is called -- the staging "
                f"implementation this lint audits does not exist."
            )
        else:
            impl = strip_comments(impl_path.read_text(encoding="utf-8", errors="replace"))
            body_match = _STAGE_FUNCTION.search(impl)
            if body_match is None:
                findings.append(
                    f"{_STAGE_IMPL}: no function(context_cef_stage_payload ...) definition found, "
                    f"though it is called from src/."
                )
            else:
                body = body_match.group(0)
                for pattern, what in _MASKING:
                    if pattern.search(body):
                        findings.append(
                            f"{_STAGE_IMPL}: context_cef_stage_payload() contains {what}. The "
                            f"concurrent copy is the defect; masking it leaves a partially staged "
                            f"output directory that fails later and far more confusingly (issue #360)."
                        )
                if "POST_BUILD" in body:
                    findings.append(
                        f"{_STAGE_IMPL}: context_cef_stage_payload() uses POST_BUILD. The staging must "
                        f"be a stamp-guarded add_custom_command(OUTPUT ...) so it re-runs only when "
                        f"the pinned payload changes -- a POST_BUILD copy is both per-target (the "
                        f"race) and unconditional (a full re-copy every build)."
                    )
                elif not re.search(r"add_custom_command\s*\(\s*OUTPUT", body):
                    findings.append(
                        f"{_STAGE_IMPL}: context_cef_stage_payload() has no "
                        f"add_custom_command(OUTPUT ...) -- without a stamp file the staging is not "
                        f"incremental."
                    )

    return findings, len(lists)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root (the directory holding src/ and cmake/). Defaults to this script's repo.",
    )
    args = parser.parse_args(argv)

    root = args.repo_root.resolve()
    findings, scanned = scan(root)

    if findings:
        print(f"CEF staging lint: {len(findings)} finding(s) over {scanned} CMakeLists.txt file(s)")
        for finding in findings:
            print(f"  FINDING {finding}")
        return 1

    print(f"CEF staging lint: clean over {scanned} CMakeLists.txt file(s)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as exc:  # a configuration error carries a message, not an int
        if isinstance(exc.code, str):
            print(exc.code, file=sys.stderr)
            sys.exit(2)
        raise
