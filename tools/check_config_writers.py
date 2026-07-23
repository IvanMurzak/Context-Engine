#!/usr/bin/env python3
"""The SINGLE-WRITER gate for `~/.context/config.json` (M9 e06d, C-F14/C-F22).

The rule the design states is one sentence — *the Shell is the single writer of the user config;
editor-core reads and requests* — and it is exactly the kind of rule that is trivially ASSERTED and
never actually checked. A comment saying "single writer" passes every test suite ever written,
including one that happily runs with a second writer present. So this is a SOURCE scan over the
repository that answers the three questions a second writer would have to make false:

  1. **How many C++ translation units write the file?**  Exactly one: `src/editor/shell/src/
     user_config.cpp`. Any other source that both names the user-config document AND performs a file
     write is a second writer, whatever its intentions. (This check FAILED before e06d: `welcome.cpp`
     had its own `ofstream` + `rename` pair, which is precisely why opening a project used to discard
     the theme — the gate is not hypothetical, it reproduces a defect that shipped.)

  2. **Can editor-core persist ANYTHING on its own?**  No. The renderer has no filesystem, so a second
     store there could only be a browser one — `localStorage`, `sessionStorage`, `indexedDB`, or a
     smuggled `node:fs`. Those are the exact shortcut a Settings-panel author reaches for
     (`localStorage.setItem("theme", id)`), it would work, and it would silently become a second
     source of truth for the user's choice. Banned outright in `src/editor/webui/**`.

  3. **Does the renderer's write REQUEST go through one door?**  The `config.set` wire method may be
     named by exactly one editor-core module (the typed client). A second module naming it is a second
     write path in all but name, and it is how "the client validates" quietly stops being true.

Exit 0 = pass. Exit 1 = a violation (each one printed with its file, line, and what to do). Exit 2 =
the scan could not run (a bad --repo-root, or an anchor file that moved), which is deliberately NOT a
pass: a gate that cannot find its subject must say so rather than report OK.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The ONE translation unit allowed to write the user config, repo-root-relative.
SOLE_WRITER = "src/editor/shell/src/user_config.cpp"

# The header that DECLARES the write primitive: it names the file and describes writing, but contains
# no write itself. Listed so the scan's "names the config + mentions writing" heuristic does not have
# to be clever about comments.
WRITER_HEADER = "src/editor/shell/include/context/editor/shell/user_config.h"

# Where C++ is scanned. The whole engine — a second writer is no less a second writer for living in
# the CLI or a test.
CPP_ROOTS = ("src",)
CPP_SUFFIXES = (".cpp", ".h", ".hpp", ".cc")

# editor-core's TypeScript workspace.
WEBUI_ROOT = "src/editor/webui"
TS_SUFFIXES = (".ts", ".tsx", ".mts", ".cts")

# Directories never scanned (build output, fetched third-party payloads).
SKIP_DIRS = {"build", "build-msvc-check", "node_modules", "vcpkg_installed", ".git"}

# A source "names the user config" when it names the document path or its dedicated helpers.
CONFIG_NAME = re.compile(r'"config\.json"|user_config_path\(|read_user_config\(|write_user_config\(')

# ...and it "writes a file" when it opens one for output or publishes one by rename. Deliberately
# syntactic: these are the only two ways to put bytes on disk in this codebase, and a scan that tried
# to be smarter would be a scan nobody could predict.
CPP_WRITE = re.compile(r"std::ofstream|ofstream\s+\w+|fs::rename\(|std::filesystem::rename\(")

# Client-side persistence APIs. A renderer cannot open a file, so THESE are what a second store in
# editor-core would be built from.
TS_PERSISTENCE = re.compile(
    r"\blocalStorage\b|\bsessionStorage\b|\bindexedDB\b|\bopenDatabase\b"
    r'|require\(\s*["\']fs["\']\s*\)|from\s+["\']node:fs["\']'
)

# The wire method that REQUESTS a write, and the one module allowed to name it.
CONFIG_SET_LITERAL = re.compile(r'"config\.set"')
CONFIG_SET_OWNER = "src/editor/webui/core/src/config.ts"


#: A comment mentioning `localStorage` is not a second store — it is usually the note explaining why
#: there ISN'T one (editorstate.ts says exactly that). Comments are blanked (not deleted) before every
#: match so line numbers stay true and a finding still points at real code.
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    return COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def is_test_source(rel: str) -> bool:
    """Is this a test/fixture source rather than shipped product code?

    Rules 1 and 3 are about what SHIPS. A test that writes its own config fixture is not a second
    writer (it authors an input, in a temp dir, then reads it back through the same code path), and a
    test that names the `config.set` literal is PINNING the wire word, not opening a second door.
    Rule 2 deliberately still applies to tests: browser-side persistence has no legitimate use here at
    all, and a test reaching for it is how the habit starts.
    """
    parts = rel.split("/")
    return "tests" in parts or "test" in parts or parts[-1].startswith("test_")


def iter_sources(root: Path, rel_roots: tuple[str, ...], suffixes: tuple[str, ...]):
    """Yield (relative_posix_path, text) for every scannable source under `rel_roots`."""
    for rel_root in rel_roots:
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in suffixes or not path.is_file():
                continue
            if any(part in SKIP_DIRS for part in path.relative_to(root).parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            yield path.relative_to(root).as_posix(), strip_comments(text)


def line_of(text: str, match_end: int) -> int:
    return text.count("\n", 0, match_end) + 1


def check(root: Path) -> list[str]:
    """Return the list of violations (empty = pass). Raises FileNotFoundError on a missing anchor."""
    for anchor in (SOLE_WRITER, WRITER_HEADER):
        if not (root / anchor).is_file():
            raise FileNotFoundError(
                f"{anchor} does not exist under {root} — the single-writer anchor moved; update "
                f"tools/check_config_writers.py to follow it (a gate that cannot find its subject "
                f"must fail, not pass)"
            )

    violations: list[str] = []

    # (1) exactly one C++ writer.
    for rel, text in iter_sources(root, CPP_ROOTS, CPP_SUFFIXES):
        if rel in (SOLE_WRITER, WRITER_HEADER) or is_test_source(rel):
            continue
        if not CONFIG_NAME.search(text):
            continue
        write = CPP_WRITE.search(text)
        if write is None:
            continue
        violations.append(
            f"{rel}:{line_of(text, write.end())}: names the user config AND writes a file directly "
            f"({write.group(0)!r}). ~/.context/config.json has exactly ONE writer (C-F14): call "
            f"write_user_config() from {SOLE_WRITER} instead — read-modify-write over the parsed "
            f"document, so members other features own are preserved."
        )

    # (2) editor-core carries no client-side persistence.
    for rel, text in iter_sources(root, (WEBUI_ROOT,), TS_SUFFIXES):
        for hit in TS_PERSISTENCE.finditer(text):
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: editor-core may not persist anything itself "
                f"({hit.group(0)!r}). It is a pure wire-client (04 §1 / 08 §1): the user config is "
                f"READ over `config.get` and CHANGED by REQUESTING `config.set`; a browser-side store "
                f"would be a second source of truth for the same user choice."
            )

    # (3) one door for the write request.
    for rel, text in iter_sources(root, (WEBUI_ROOT,), TS_SUFFIXES):
        if rel == CONFIG_SET_OWNER or is_test_source(rel):
            continue
        hit = CONFIG_SET_LITERAL.search(text)
        if hit is not None:
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: names the `config.set` wire method directly. "
                f"Only {CONFIG_SET_OWNER} may — every other module goes through its typed client, so "
                f"the request shape (and its validation) has one definition."
            )

    return violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="the Context-Engine repository root")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        print(f"[config-writers] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        violations = check(root)
    except FileNotFoundError as exc:
        print(f"[config-writers] ERROR: {exc}", file=sys.stderr)
        return 2

    if violations:
        for v in violations:
            print(f"[config-writers] FINDING: {v}", file=sys.stderr)
        print(
            f"[config-writers] FAIL: {len(violations)} single-writer violation(s) — "
            f"the Shell is the SINGLE writer of ~/.context/config.json (C-F14)",
            file=sys.stderr,
        )
        return 1

    print("[config-writers] PASS: one C++ writer, no client-side persistence in editor-core, "
          "one `config.set` caller")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
