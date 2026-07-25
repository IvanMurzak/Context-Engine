#!/usr/bin/env python3
"""The SESSION-FILE OWNERSHIP gate (M9 e09d, design 03 §1 / review C-F3).

Two session files sit side by side in a project's `.editor/` directory, and they have two DIFFERENT
owners:

  * `.editor/session.json`      — the DAEMON's (selection, cameras, play state);
  * `.editor/editor-state.json` — the editor SHELL's (dock layout, window placement, panel state
    blobs, the session undo journal, the editor presence marker).

One writer per file is the whole design. Two processes writing one file is a torn write nobody
detects until a user's window layout — or, since e09c, their undo history — comes back mangled, and
the failure is invisible in every ordinary test because it needs two processes racing to appear at
all. That is exactly the class of rule that gets ASSERTED IN A COMMENT and never actually checked: a
header saying "single writer" passes every suite ever written, INCLUDING one run against a tree that
has grown a second writer. So this is a SOURCE scan that answers the questions a second writer would
have to make false:

  1. **How many C++ translation units write `.editor/session.json`?**  Exactly one. Any other source
     that both names that document AND performs a file write is a second writer, whatever its
     intentions.

  2. **How many write `.editor/editor-state.json`?**  Exactly one, for the same reason.

  3. **Do the two owners live in the two different PROCESSES?**  Rules 1 and 2 would both still pass
     if someone moved the editor-state writer into the daemon: one writer per file, but the SPLIT
     dead. So the owners are pinned to their subtrees — `src/editor/editorkernel/` (the daemon) and
     `src/editor/shell/` (the editor app) — which is the property C-F3 actually bought.

  4. **Can the in-process compose/filesync override-write gateway be reached from the live path?**
     No. `ProjectOverrideWriteGateway` drives `compose::plan_write` + filesync's atomic write
     IN-PROCESS; since e09b-2 the live editor commits over the daemon's `edit` RPC instead
     (`WireOverrideWriteGateway`), and the in-process one remains ONLY as the injected T1 mock's
     real-disk sibling (parent e09 scope: "the in-process compose/filesync gateway remains ONLY for
     headless T1 tests"). Two halves: any NON-test source NAMING it is that gateway back on the live
     path, and no Shell target may LINK the library it lives in (a scan of the CMake files, since this
     task's DoD requires `src/CMakeLists.txt` — where the D10 FORBIDDEN set lives — to stay
     byte-identical; see GATEWAY_LIBRARY for why the two asserts are complementary, not redundant).

  5. **Are the anchors still real?**  Each sole writer must still exist, still NAME its file, and
     still CONTAIN a write. A gate whose subject evaporated must fail, not pass: the prohibitions in
     1-4 are all trivially satisfied by a tree that deleted the thing they protect.

WHAT THIS DELIBERATELY DOES NOT RE-CHECK. `tools/check_config_writers.py` already bans client-side
persistence (`localStorage` / `sessionStorage` / `indexedDB` / `node:fs`) across ALL of
`src/editor/webui/**`, which is the only way editor-core could keep a second copy of either session
file — the renderer has no filesystem. Restating it here would report one violation twice and let the
two spellings drift apart. The editor-core side of C-F3 is that gate's rule 2.

Exit 0 = pass. Exit 1 = a violation (each printed with its file, line, and what to do instead).
Exit 2 = the scan could not run (a bad --repo-root, or an anchor that moved), which is deliberately
NOT a pass: a gate that cannot find its subject must say so rather than report OK.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------------- the anchors

# file label -> (the ONE translation unit allowed to write it, the subtree its OWNER PROCESS lives in)
SOLE_WRITERS = {
    ".editor/session.json": (
        "src/editor/editorkernel/src/editor_session_state.cpp",
        "src/editor/editorkernel/",
    ),
    ".editor/editor-state.json": (
        "src/editor/shell/src/editor_state.cpp",
        "src/editor/shell/",
    ),
}

# The headers that DECLARE each write primitive: they name the document and describe writing, but
# contain no write themselves. Listed so the "names the file + writes" heuristic does not have to be
# clever about a declaration.
WRITER_HEADERS = {
    ".editor/session.json": (
        "src/editor/editorkernel/include/context/editor/editorkernel/editor_session_state.h"
    ),
    ".editor/editor-state.json": "src/editor/shell/include/context/editor/shell/editor_state.h",
}

# The on-disk DOCUMENT NAME itself, wherever it appears in source text.
#
# ⚠ THE LOOKBEHIND IS THE WHOLE POINT, AND THE FIRST REVISION GOT IT WRONG. That revision matched the
# name WITH ITS OPENING QUOTE (`"editor-state\.json"`), which reads as safely anchored and was
# defeated by THREE ordinary spellings in the first planting round — none of them evasive, all of them
# things a person writes without thinking:
#
#     r / ".editor/editor-state.json"        the whole relative path as ONE literal
#     r / R"(editor-state.json)"             a raw string literal (the quote is not adjacent)
#     r / ".editor\\editor-state.json"       a Windows-style separator
#
# So the name is matched WHEREVER it appears, and the lookbehind instead rejects the ways it can be a
# DIFFERENT file: a preceding word character, `.` or `-` means this is `snapshot.session.json` (the
# runtime's `session *` FILE-harness family, which C-F4 keeps deliberately distinct) or
# `my-editor-state.json`, not the document these rules own. Comments are blanked before any match, so
# the many headers that discuss these files by name are not hits.
FILE_LITERALS = {
    ".editor/session.json": re.compile(r"(?<![\w.\-])session\.json"),
    ".editor/editor-state.json": re.compile(r"(?<![\w.\-])editor-state\.json"),
}

# A source "names" each session file when it names the document (above) OR reaches it through one of
# its dedicated path/IO helpers — the spelling a second writer inside this codebase would actually
# use, since the path is already available as a function call.
FILE_NAMES = {
    ".editor/session.json": re.compile(
        FILE_LITERALS[".editor/session.json"].pattern
        + r"|\bsession_state_path\s*\("
        + r"|\brestore_session_state\s*\("
        + r"|\bpersist_session_state\s*\("
    ),
    ".editor/editor-state.json": re.compile(
        FILE_LITERALS[".editor/editor-state.json"].pattern
        + r"|\beditor_state_path\s*\("
        + r"|\beditor_state_quarantine_path\s*\("
    ),
}

# ...and it "writes a file" when it opens one for output or publishes one by rename/copy.
# Deliberately syntactic: a scan that tried to be smarter would be a scan nobody could predict.
#
# ⚠ THE SPELLING LIST IS THE WHOLE GATE, and this one is INHERITED FROM A MEASURED FAILURE. The first
# revision of the sibling `check_config_writers.py` listed only `std::ofstream` / `ofstream <var>` /
# `fs::rename(` / `std::filesystem::rename(`, and a review that PLANTED second writers (rather than
# reading the regex) found FOUR spellings that sailed straight through: a `std::fstream` opened with
# `std::ios::out`, `std::filesystem::copy_file`, C stdio `std::fopen(p, "w")`, and the C library's own
# `std::rename`. Every one is an ordinary way to put bytes on disk that an author could reach for with
# no intent to evade anything. So the rule for editing this list is: a spelling comes OUT only with
# evidence it cannot write, and any new way to write a file that lands in this codebase goes IN with a
# `tools/tests/` case that PLANTS it.
#
# `tools/tests/test_check_session_ownership.py` additionally pins this list against
# `check_config_writers.CPP_WRITE`, so the two gates cannot silently drift apart: a spelling learned
# by one is a spelling the other must know.
#
# Being generous here is deliberately cheap: a hit only becomes a finding in a file that ALSO names
# one of the two session files (see `check`), so the blast radius of an over-broad token is the
# handful of session-file translation units, where "prove this is not a second writer" is the posture
# we want anyway.
#
# TWO patterns, not one, and the split is a planting finding. The PRIMITIVE list is the set of
# language/stdlib spellings that actually put bytes on disk; `CPP_WRITE` adds the repo's own write
# WRAPPERS on top. Rules 1-2 (is this a second writer?) use the wide one — calling `atomic_write` on
# a session file is a second writer as surely as opening an `ofstream` on it. The anti-vacuity half
# (does the declared sole writer still write?) uses the PRIMITIVE one, because a wrapper name is
# satisfied by the writer's own helper DEFINITION: with the wide pattern, a plant that stripped every
# real write out of `editor_state.cpp` still left `atomic_write_text(` behind and the gate reported
# PASS — the exact vacuity that check was added to catch.
CPP_WRITE_PRIMITIVE = re.compile(
    r"std::ofstream"          # the common declaration
    r"|\bofstream\s+\w+"      # ...and an unqualified one
    r"|\bbasic_ofstream\b"    # the underlying template, named directly
    r"|\bfstream\b"           # std::fstream opened with ios::out (NOT matched by \bofstream\b)
    r"|\brename\("            # fs::rename / std::filesystem::rename / the C library's std::rename
    r"|\bcopy_file\("         # std::filesystem::copy_file over the destination
    r"|\bcopy_options\b"      # std::filesystem::copy with overwrite_existing (omits `copy_file`)
    r"|\bfopen\(|\bfreopen\(" # C stdio, opened "w"/"a"
)
CPP_WRITE = re.compile(
    CPP_WRITE_PRIMITIVE.pattern
    + r"|\bwrite_file\w*\s*\("    # the repo's own test/helper write wrappers
    + r"|\batomic_write\w*\s*\("  # filesync's R-FILE-004 primitive, reached directly
)
# ...deliberately WITHOUT the C stdio write calls themselves (`fwrite`/`fputs`/`fprintf`). They look
# like the obvious thing to add and are actively wrong here: `std::fprintf(stderr, …)` is this
# codebase's ordinary logging idiom — and BOTH session-file owners log their recovery through it — so
# including them would report the sole writers' own diagnostics as writes while catching nothing new,
# because a C-stdio write to a file needs an `fopen`/`freopen` in the same translation unit anyway (a
# FILE* arriving as a parameter is outside any syntactic scan's reach, by construction — the same
# documented limitation `check_config_writers.py` carries).

# ------------------------------------------------------------------------- the documented READERS

# A source may legitimately NAME a session file it does not own — the split forbids a second WRITER,
# not a reader — while ALSO writing some OTHER file in the same translation unit. The scan is
# syntactic and cannot tell those two paths apart, so each such source is listed here ONCE, with the
# reason, and reviewed on the way in. Both obligations below are then re-checked on EVERY run, so an
# entry cannot rot into a licence:
#
#   * it must STILL name the document (a stale entry is deleted, not left as a standing exemption);
#   * it must NOT name that file's OWNING WRITE API (`OWNER_WRITE_API` below). A reader that has
#     started reaching for the owner's store is no longer a reader, and that is the one way an entry
#     here could quietly become the second writer it was granted for not being.
#
# Adding an entry is a claim you must be able to defend from the source: "this file READS the
# document and writes only its own sibling artifact".
DOCUMENTED_READERS = {
    "src/editor/client/src/arbitration.cpp": (
        ".editor/editor-state.json",
        "e14b single-instance arbitration: READS the presence marker out of the editor-state "
        "DOCUMENT TEXT and writes only the SEPARATE `.editor/focus-request` handshake file — "
        "arbitration.h states the split in its own header ('never writes editor-state.json').",
    ),
}

# The owning store / write entrypoints for each file. A documented reader naming one of these has
# stopped being a reader.
OWNER_WRITE_API = {
    ".editor/session.json": re.compile(r"\bpersist_session_state\s*\("),
    ".editor/editor-state.json": re.compile(r"\bEditorStateStore\b"),
}

# --------------------------------------------------------- the in-process gateway (parent e09 scope)

# The type that IS the in-process compose/filesync write path, and the two files entitled to name it.
INPROCESS_GATEWAY = re.compile(r"\bProjectOverrideWriteGateway\b|\bproject_override_gateway\.h\b")
GATEWAY_OWNERS = (
    "src/editor/gui/viewport/include/context/editor/gui/viewport/project_override_gateway.h",
    "src/editor/gui/viewport/src/project_override_gateway.cpp",
)

# The LINK half of the same rule, scanned from the CMake files (the tier `check_cef_staging.py`
# occupies). The gateway lives in `context_gui_viewport_edit`; no Shell target may link it.
#
# WHY THIS IS SCANNED HERE RATHER THAN ADDED TO THE D10 AUDIT. The right home for a link rule is
# `context_assert_shell_boundary`'s FORBIDDEN set in `src/CMakeLists.txt`, and the one-line addition
# was written and then REVERTED: this task's Definition of Done requires that file to stay
# BYTE-IDENTICAL (the standing M9 guard against a task relaxing the D10 boundary to make its own code
# link). So the assert lives here instead, and it is complementary rather than redundant:
#
#   * the EXISTING audit already forbids `context_compose` + `context_filesync` transitively, which
#     catches the gateway TODAY because its library links both — but only for as long as its
#     implementation keeps them;
#   * this rule names the SUBJECT, so the failure message is about the in-process gateway rather than
#     about a compose dependency two hops away, and it survives a refactor that moved the in-process
#     write behind an indirection.
#
# Syntactic, so it sees DIRECT links only; the transitive case stays the closure audit's job. Adding
# `context_gui_viewport_edit` to that FORBIDDEN set remains the stronger mechanism and is the right
# follow-up for a task permitted to touch `src/CMakeLists.txt`.
GATEWAY_LIBRARY = "context_gui_viewport_edit"
SHELL_LINK_TARGETS = ("context_editor_shell", "context_editor", "context_editor_panels")
CMAKE_ROOTS = ("src",)
CMAKE_NAMES = ("CMakeLists.txt",)
# `target_link_libraries(<target> ...)` up to its closing paren — CMake has no nesting here.
TARGET_LINK = re.compile(r"target_link_libraries\s*\(\s*([A-Za-z0-9_]+)([^)]*)\)", re.DOTALL)

# Where C++ is scanned. The whole engine — a second writer is no less a second writer for living in
# the CLI, and the gateway is no less on the live path for being reached from a package.
CPP_ROOTS = ("src",)
CPP_SUFFIXES = (".cpp", ".h", ".hpp", ".cc", ".mm")

# Directories never scanned (build output, fetched third-party payloads).
SKIP_DIRS = {"build", "build-msvc-check", "node_modules", "vcpkg_installed", ".git"}

#: A comment mentioning `editor-state.json` is not a second writer — it is usually the note explaining
#: why there ISN'T one (`panelhost.ts` and `undo_journal.h` both say exactly that, and this repo
#: comments heavily). Comments are blanked (not deleted) before every match so line numbers stay true
#: and a finding still points at real code.
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

#: `#include <fstream>` is not a write, and the generous `\bfstream\b` token in CPP_WRITE matches it.
#: That is not academic: it produced BOTH directions of wrongness in one planting round. It reported
#: `arbitration.cpp` line 7 — the include line — as a "second writer", and it kept the anti-vacuity
#: half GREEN against a plant that had removed every actual write from `editor_state.cpp`, because
#: the include alone still satisfied "this file writes".
#:
#: So includes are blanked for the WRITE search ONLY (`find_write`), never globally: rule 4's whole
#: subject is an `#include "…/project_override_gateway.h"`, and blanking includes everywhere would
#: have quietly retired that rule while its plant still reported RED for the other half of the
#: pattern. Two texts, one blanking helper, each rule reading the one it means.
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[^\n]*", re.MULTILINE)


def _blank(match: re.Match) -> str:
    """Replace a span with spaces, keeping newlines — so offsets (and line numbers) stay true."""
    return re.sub(r"[^\n]", " ", match.group(0))


def strip_comments(text: str) -> str:
    return COMMENT.sub(_blank, text)


def find_write(text: str, pattern: "re.Pattern[str]" = CPP_WRITE):
    """The first FILE-WRITE spelling in comment-stripped `text`, ignoring `#include` lines."""
    return pattern.search(INCLUDE.sub(_blank, text))


def is_test_source(rel: str) -> bool:
    """Is this a test/fixture source rather than shipped product code?

    Rules 1-3 are about what SHIPS. A test that authors a corrupt `session.json` fixture, or writes an
    `editor-state.json` carrying a presence marker to read back, is not a second writer: it builds an
    INPUT in a temp dir and then drives the real code path over it, which is how the recovery
    behaviour is asserted at all.

    Rule 4 needs tests exempt for a stronger reason: "reachable ONLY from T1 mocks" makes the tests
    the one legitimate caller, so exempting them is not a loophole in the rule — it IS the rule.

    ⚠ SMOKES COUNT AS TESTS, and that is measured rather than assumed. `src/editor/shell/smoke/` and
    the `*_smoke.cpp` TUs under `src/editor/shell/cef/src/` are ctest-registered executables
    (`editor-shell-smoke-session0`, `editor-cef-smoke-shell*`, `editor-shell-x11-window`) that ship in
    no artifact; they author oracle/fixture files beside the real ones exactly as a `tests/` source
    does. Landing this gate without them reported `cef_shell_restore_smoke.cpp` — which writes a
    LAYOUT ORACLE for its own phase 2 and merely asserts the existence of the editor-state file — as a
    second writer of a file it never opens.
    """
    parts = rel.split("/")
    if "tests" in parts or "test" in parts or "smoke" in parts:
        return True
    name = parts[-1]
    return name.startswith("test_") or "_smoke" in name


def iter_sources(root: Path, rel_roots: tuple[str, ...], suffixes: tuple[str, ...]):
    """Yield (relative_posix_path, comment-stripped text) for every scannable source."""
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
    anchors = [w for w, _ in SOLE_WRITERS.values()]
    anchors += list(WRITER_HEADERS.values())
    anchors += list(GATEWAY_OWNERS)
    anchors += list(DOCUMENTED_READERS)
    for anchor in anchors:
        if not (root / anchor).is_file():
            raise FileNotFoundError(
                f"{anchor} does not exist under {root} — a session-ownership anchor moved; update "
                f"tools/check_session_ownership.py to follow it (a gate that cannot find its "
                f"subject must fail, not pass)"
            )

    violations: list[str] = []
    exempt = set(SOLE_WRITERS[label][0] for label in SOLE_WRITERS)
    exempt |= set(WRITER_HEADERS.values())

    sources = list(iter_sources(root, CPP_ROOTS, CPP_SUFFIXES))
    by_rel = dict(sources)

    # (5) THE POSITIVE HALF, checked first: each sole writer still names its file and still writes.
    # Without this, deleting the write from `editor_state.cpp` would make rules 1-3 pass perfectly.
    #
    # ⚠ IT KEYS ON THE DOCUMENT LITERAL, NOT `FILE_NAMES`, and that is a planting finding too. The
    # first revision used FILE_NAMES here, which also matches the file's own path/IO HELPERS — and a
    # writer TU always contains those, because it DEFINES them. So the check could not fail: pointing
    # `session_state_path` at a different filename left the gate green while the daemon's session file
    # had silently moved. The literal is the only half of that pattern a redirect actually removes.
    for label, (writer, owner_subtree) in SOLE_WRITERS.items():
        text = by_rel.get(writer, "")
        if FILE_LITERALS[label].search(text) is None:
            violations.append(
                f"{writer}: no longer names {label} — the declared sole writer does not write it, so "
                f"this gate would pass vacuously. Point SOLE_WRITERS at the real writer."
            )
        if find_write(text, CPP_WRITE_PRIMITIVE) is None:
            violations.append(
                f"{writer}: no longer performs a file write — the declared sole writer of {label} "
                f"writes nothing, so this gate would pass vacuously. Point SOLE_WRITERS at the real "
                f"writer."
            )
        # (3) the SPLIT itself: the owner lives in its own process's subtree.
        if not writer.startswith(owner_subtree):
            violations.append(
                f"{writer}: the sole writer of {label} must live under {owner_subtree} — the C-F3 "
                f"split is not 'one writer per file', it is 'one writer per file, in THAT file's "
                f"OWNING PROCESS' (design 03 §1)."
            )

    # (5b) the DOCUMENTED READERS' own two obligations — checked before they are used as exemptions,
    # so an entry that has gone stale (or gone rogue) is reported rather than silently honoured.
    for rel, (label, reason) in DOCUMENTED_READERS.items():
        text = by_rel.get(rel, "")
        if FILE_NAMES[label].search(text) is None:
            violations.append(
                f"{rel}: is listed as a documented READER of {label} but no longer names it — the "
                f"exemption is stale. DELETE the DOCUMENTED_READERS entry rather than leaving a "
                f"standing licence behind (reason on record: {reason})"
            )
        owner_api = OWNER_WRITE_API.get(label)
        if owner_api is not None:
            hit = owner_api.search(text)
            if hit is not None:
                violations.append(
                    f"{rel}:{line_of(text, hit.end())}: is a documented READER of {label} but names "
                    f"that file's OWNING WRITE API ({hit.group(0)!r}). A reader that reaches for the "
                    f"owner's store is no longer a reader — either route the change through "
                    f"{SOLE_WRITERS[label][0]} or drop the DOCUMENTED_READERS exemption."
                )

    # (1)+(2) exactly one C++ writer per session file.
    for label, (writer, owner_subtree) in SOLE_WRITERS.items():
        other_process = [s for lab, (_, s) in SOLE_WRITERS.items() if lab != label]
        for rel, text in sources:
            if rel in exempt or is_test_source(rel):
                continue
            if DOCUMENTED_READERS.get(rel, (None, None))[0] == label:
                continue # a reviewed reader — its own two obligations are checked just above
            if FILE_NAMES[label].search(text) is None:
                continue
            write = find_write(text)
            if write is None:
                continue
            cross = any(rel.startswith(s) for s in other_process)
            direction = (
                f" This is a CROSS-PROCESS write: {rel} lives in the OTHER owner's subtree, so it is "
                f"a second process writing {label} — the torn write the split exists to prevent."
                if cross
                else ""
            )
            violations.append(
                f"{rel}:{line_of(text, write.end())}: names {label} AND writes a file directly "
                f"({write.group(0)!r}). That file has exactly ONE writer (C-F3, design 03 §1): "
                f"{writer}. Route the change through it instead.{direction}"
            )

    # (4b) ...and no Shell target LINKS the library it lives in (the CMake half of rule 4).
    for rel, text in iter_sources(root, CMAKE_ROOTS, (".txt",)):
        if Path(rel).name not in CMAKE_NAMES:
            continue
        for call in TARGET_LINK.finditer(text):
            target, payload = call.group(1), call.group(2)
            if target not in SHELL_LINK_TARGETS:
                continue
            if re.search(rf"\b{re.escape(GATEWAY_LIBRARY)}\b", payload) is None:
                continue
            violations.append(
                f"{rel}:{line_of(text, call.start())}: {target} links {GATEWAY_LIBRARY}, the library "
                f"holding the IN-PROCESS compose/filesync override-write gateway. The live editor "
                f"commits over the daemon's `edit` RPC (shell::panels::WireOverrideWriteGateway, "
                f"e09b-2); that gateway is the injected T1 mock's real-disk sibling and must stay off "
                f"the Shell's link closure (parent e09 DoD; the D10 audit in src/CMakeLists.txt "
                f"forbids its context_compose/context_filesync dependencies transitively)."
            )

    # (4) the in-process compose/filesync gateway stays off the live path.
    for rel, text in sources:
        if rel in GATEWAY_OWNERS or is_test_source(rel):
            continue
        hit = INPROCESS_GATEWAY.search(text)
        if hit is not None:
            violations.append(
                f"{rel}:{line_of(text, hit.end())}: names the IN-PROCESS override-write gateway "
                f"({hit.group(0)!r}) from a non-test source. It drives compose::plan_write + "
                f"filesync's atomic write IN THIS PROCESS, and the live editor must commit over the "
                f"daemon's `edit` RPC instead (shell::panels::WireOverrideWriteGateway, e09b-2). It "
                f"remains ONLY as the injected T1 mock's real-disk sibling (parent e09 scope)."
            )

    return violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="the Context-Engine repository root")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        print(f"[session-ownership] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        violations = check(root)
    except FileNotFoundError as exc:
        print(f"[session-ownership] ERROR: {exc}", file=sys.stderr)
        return 2

    if violations:
        for v in violations:
            print(f"[session-ownership] FINDING: {v}", file=sys.stderr)
        print(
            f"[session-ownership] FAIL: {len(violations)} ownership violation(s) — the daemon is the "
            f"SINGLE writer of .editor/session.json and the Shell is the SINGLE writer of "
            f".editor/editor-state.json (C-F3, design 03 §1)",
            file=sys.stderr,
        )
        return 1

    print("[session-ownership] PASS: one writer per session file, each in its owning process, and "
          "the in-process override-write gateway is test-only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
