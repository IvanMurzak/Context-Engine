#!/usr/bin/env python3
"""Fail on any code that drives, or enables, CEF's external BeginFrame pacing (L-41 / cef#4033).

L-41 pins the Shell to CEF-INTERNAL frame pacing: the compositor decouples engine FPS from CEF's own
60 Hz, so nothing in this repo may call `SendExternalBeginFrame` or turn on the window-info flag that
puts CEF into externally-paced mode. Upstream cef#4033 makes that mode unreliable, and the failure it
produces is not a crash — it is a browser that silently stops painting under load, which reads as a
Shell bug and is diagnosable only from a live CEF leg.

THE PROBLEM THIS SOLVES. There is no call site today, so no runtime test can fail if one appears: an
assertion can only observe behaviour that EXISTS. The commitment is therefore enforced from the
SOURCES, the same tier as tools/check_cef_staging.py / tools/check_config_writers.py, which is also
what makes it run on every default `build` leg with no CEF in the build at all.

WHAT IT CHECKS (over comment-stripped source text, so the many prose mentions of the API — in
surface.h, cef_shell.h, docs, and this file's own callers — never count as uses):

  1. NO CALL SITE, AND NO ROUTE TO ONE. The identifier `SendExternalBeginFrame` must not appear in
     any scanned source file. Not a call shape, the IDENTIFIER: `host->SendExternalBeginFrame()`, a
     chained `browser->GetHost()->SendExternalBeginFrame()`, a member-function pointer that names it
     without ever writing a paren, and a string literal a runtime lookup would build a selector from
     are all the same finding. A rule that matched `\\bSendExternalBeginFrame\\s*\\(` instead would
     pass every one of the last three.

  2. THE ENABLING FLAG IS NEVER TURNED ON. `CefWindowInfo::external_begin_frame_enabled` is what puts
     a browser into externally-paced mode in the first place; a build that sets it and then never
     calls the API has a browser that waits forever for a frame nobody sends. Any assignment or
     designated-initialisation of an `external_begin_frame`-named member to a true value (`true`,
     `1`, `TRUE`, `YES`) is a finding, in either the `x.f = true` or the C++20 `.f = true` spelling.

  3. THE PIN ITSELF IS STILL THERE, AND STILL FALSE (the SINK-side rule). Checks 1 and 2 are
     PROHIBITIONS, and a prohibition passes trivially against a tree that deleted the thing it was
     protecting. So the positive half is checked at the one place the value is decided:
     `src/editor/gui/compositor/src/surface.cpp` must assign `external_begin_frame` exactly once, and
     must assign it `false`. Deleting the line, or flipping it, is a finding even though checks 1 and
     2 would both stay green — and a construction site that aggregate-initialises the handoff
     positionally, which no name-keyed pattern can see, still has to come through this sink.

Exit codes: 0 = clean, 1 = at least one finding, 2 = a configuration error.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# The banned API, by NAME. Deliberately not a call-shaped pattern -- see the docstring, check 1.
_BANNED_API = re.compile(r"\bSendExternalBeginFrame\b")

# An `external_begin_frame`-named member being set to a true value. Both spellings of the assignment
# a caller can write: `window_info.external_begin_frame_enabled = true` and the C++20 designated
# initialiser `.external_begin_frame = true`. The `[A-Za-z_]*` tail is what makes
# `external_begin_frame_enabled` and any future sibling suffix match the same rule.
_ENABLED_TRUE = re.compile(
    r"\bexternal_begin_frame[A-Za-z_]*\s*(?:=|\{)\s*(?:true|TRUE|YES|1)\b"
)
# The setter-call spelling: `set_external_begin_frame(true)` / `.external_begin_frame(true)`.
_ENABLED_TRUE_CALL = re.compile(
    r"\b(?:set_)?external_begin_frame[A-Za-z_]*\s*\(\s*(?:true|TRUE|YES|1)\s*\)"
)

# The sink: where the handoff's value is DECIDED.
_PIN_FILE = "src/editor/gui/compositor/src/surface.cpp"
_PIN_ASSIGNMENT = re.compile(r"\bexternal_begin_frame\s*=\s*([A-Za-z0-9_]+)")

_SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".m", ".mm",
                              ".ts", ".tsx", ".js", ".mjs", ".cjs"})

# Never SOURCE. `_deps`/`_cef` hold fetched third-party trees (the CEF distribution ships its own
# `SendExternalBeginFrame` declarations, which are none of this repo's business); a generated CMake
# binary tree is identified by the CMakeCache.txt at its root, because the repo has a REAL
# `src/editor/build/` module and pruning by the name "build" would skip it.
_PRUNED_DIRS = frozenset({"_deps", "_cef", "CMakeFiles", "node_modules"})


def strip_comments(text: str) -> str:
    """Remove C/C++/JS `//` and `/* */` comments, honouring string and char literals.

    Load-bearing: `SendExternalBeginFrame` is NAMED in prose all over this repo -- the L-41 note in
    compositor/surface.h, the LIFETIME comment in cef_shell.h, the test comments that assert the
    handoff is false. A scan that did not strip comments would report every one of them and be
    switched off within a day, which is the failure mode of a gate nobody can keep green.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                # Newlines are preserved so a finding's line number stays true.
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i = min(i + 2, n)
            continue
        if ch in "\"'":
            # A LITERAL MUST CLOSE ON ITS OWN LINE, or the quote is not a literal at all. Without
            # this rule an unpaired quote sends the scan hunting to EOF and the ENTIRE remainder of
            # the file silently bypasses comment stripping. Real shapes in this tree do that: a C++14
            # digit separator (`should_pump(4'010)`), an apostrophe inside a JSON description string
            # or a TS template literal. The failure direction is not a missed violation — literal
            # content is copied through, so the prohibitions still see an identifier — it is a FALSE
            # RED on any later comment that merely MENTIONS the API in prose, which this file's own
            # docstring calls out as the failure mode of a gate nobody can keep green. Measured at 8
            # files before this guard. A backslash-newline still continues a literal (the newline
            # lands in the slice, so reported line numbers stay true).
            quote = ch
            end = i + 1
            while end < n and text[end] != quote and text[end] != "\n":
                end += 2 if text[end] == "\\" and end + 1 < n else 1
            if end >= n or text[end] != quote:
                out.append(ch)  # an ordinary character, not a delimiter
                i += 1
                continue
            out.append(text[i : end + 1])
            i = end + 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def source_files(src: Path) -> list[Path]:
    """Every AUTHORED source file under src/, with generated and vendored subtrees pruned."""
    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(src):
        directory = Path(dirpath)
        dirnames[:] = [
            d
            for d in dirnames
            if d not in _PRUNED_DIRS
            and not d.startswith(".")
            and not (directory / d / "CMakeCache.txt").is_file()
        ]
        for filename in filenames:
            if Path(filename).suffix in _SOURCE_SUFFIXES:
                found.append(directory / filename)
    return sorted(found)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def scan(repo_root: Path) -> tuple[list[str], int]:
    """Return (findings, files_scanned). Raises SystemExit(2) on a configuration error."""
    src = repo_root / "src"
    if not src.is_dir():
        raise SystemExit(
            f"check_no_external_begin_frame: no src/ directory under {repo_root} (bad --repo-root?)"
        )

    files = source_files(src)
    if not files:
        raise SystemExit(f"check_no_external_begin_frame: no source files found under {src}")

    findings: list[str] = []
    pin_seen = False

    for path in files:
        relative = path.relative_to(repo_root).as_posix()
        stripped = strip_comments(path.read_text(encoding="utf-8", errors="replace"))

        # --- check 1: the banned API is not named at all ------------------------------------------
        for match in _BANNED_API.finditer(stripped):
            findings.append(
                f"{relative}:{_line_of(stripped, match.start())}: names SendExternalBeginFrame "
                f"outside a comment. L-41 pins the Shell to CEF-INTERNAL pacing (cef#4033): "
                f"externally-paced OSR silently stops painting under load. Let CEF pace itself and "
                f"decouple engine FPS in the compositor, as WindowCompositor already does."
            )

        # --- check 2: the enabling flag is never turned on -----------------------------------------
        for pattern in (_ENABLED_TRUE, _ENABLED_TRUE_CALL):
            for match in pattern.finditer(stripped):
                findings.append(
                    f"{relative}:{_line_of(stripped, match.start())}: sets an "
                    f"external_begin_frame flag TRUE ({match.group(0).strip()}). That is the half "
                    f"that puts CEF into externally-paced mode; a build that enables it and never "
                    f"sends a frame has a browser that waits forever. L-41 forbids both halves."
                )

        # --- check 3: the pin is still present, and still false ------------------------------------
        if relative == _PIN_FILE:
            assignments = _PIN_ASSIGNMENT.findall(stripped)
            pin_seen = True
            if len(assignments) != 1:
                findings.append(
                    f"{relative}: expected EXACTLY ONE assignment to external_begin_frame (the L-41 "
                    f"pin), found {len(assignments)}. This is the sink every construction of a "
                    f"SurfaceHandoff passes through, so it is the one place the value can be checked "
                    f"without depending on how the struct was built."
                )
            elif assignments[0] != "false":
                findings.append(
                    f"{relative}: the L-41 pin assigns external_begin_frame = {assignments[0]}, not "
                    f"false. CEF-internal pacing is the only sanctioned mode (cef#4033)."
                )

    if not pin_seen:
        findings.append(
            f"{_PIN_FILE} was not scanned: the L-41 pin it holds is what makes checks 1 and 2 more "
            f"than a pair of prohibitions that pass against a tree which deleted the thing they "
            f"protect. If the file moved, update _PIN_FILE in this gate in the SAME change."
        )

    return findings, len(files)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root (the directory holding src/). Defaults to this script's repo.",
    )
    args = parser.parse_args(argv)

    root = args.repo_root.resolve()
    findings, scanned = scan(root)

    if findings:
        print(f"external-BeginFrame lint: {len(findings)} finding(s) over {scanned} source file(s)")
        for finding in findings:
            print(f"  FINDING {finding}")
        return 1

    print(f"external-BeginFrame lint: clean over {scanned} source file(s)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as exc:  # a configuration error carries a message, not an int
        if isinstance(exc.code, str):
            print(exc.code, file=sys.stderr)
            sys.exit(2)
        raise
