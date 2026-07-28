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

  4. macOS APP-BUNDLE PAYLOADS (M9 e12c-1, issue #436). macOS does not stage beside the executable at
     all: `COPY_MAC_FRAMEWORK()` embeds the 304 MB framework INSIDE each `.app`, and each
     per-process-type helper bundle is copied into that same `Contents/Frameworks`. So an app bundle's
     `Contents/Frameworks` is a staging destination in exactly the sense check 1 means, and it gets the
     same rule: ONE writing target per app bundle (all its POST_BUILD copies then serialise inside that
     one target, instead of racing across targets), the framework embedded by the app's OWN target
     rather than by a helper or a sibling, and no bundle carrying helpers but no framework — which
     cannot boot, because `CefScopedLibraryLoader` finds nothing to load.

  5. THE ROSTER IS COMPLETE (M9 x11). `_ctx_cef_shell_executables` in src/editor/shell/CMakeLists.txt
     is the ONE hand-written list of CEF-hosting Shell executables, and BOTH configure-time audits
     there iterate THE LIST — so a target missing from it is not flagged, it is SKIPPED, and that has
     already happened twice (uimirror, iframe). This check ties the list to the LITERAL
     `add_dependencies(<exe> <stage target>)` edges check 2 already reads, and requires the two sets
     to be EQUAL: a stage consumer absent from the roster is under-audited, and a roster entry that
     takes no stage edge is stale. That the roster is non-EMPTY is asserted too — an empty list would
     satisfy "every listed target is a consumer" trivially, which is the vacuous direction.
     Its GRAPH-tier companion lives in src/editor/shell/CMakeLists.txt and derives the same roster
     from target properties under a CEF-ON configure. Two tiers, because neither is sufficient alone:
     this one runs on every default CEF-FREE `build` leg but can only read LITERAL names (see the
     `${variable}` lesson below), while the graph tier sees what CMake will really generate but exists
     only where CEF is ON.

TWO THINGS THIS FILE LEARNED THE HARD WAY, both from e12c-1, and both about the checks ABOVE being
silently INAPPLICABLE rather than wrong:

  * PLATFORM CONDITIONS ARE PART OF THE GRAPH. `context_cef_stage_payload()` is called under
    `if(OS_WINDOWS OR OS_LINUX)`, so the stage target does not exist on macOS at all — and a macOS CEF
    executable must NOT depend on it. Reading the file as a flat list of call sites made check 2
    demand exactly that, which reds `editor-shell-cef-staging` on ALL THREE legs the moment the first
    macOS CEF target appears. Every call site therefore carries the platform set of its enclosing
    `if`/`elseif` chain, and checks 1/2/4 only fire where two platform sets actually INTERSECT.

  * A `${variable}` TARGET NAME IS STILL A TARGET. The two older macOS branches
    (src/editor/cef/, src/editor/gui/host/) name every target through a `${var}`, and the original
    pattern matched literal names only — so those targets were not "correct", they were INVISIBLE, and
    the lint was silently under-covering the whole macOS half of the tree. Names are now resolved
    against file-local `set()` assignments before anything is compared, and a name that STILL cannot be
    resolved (one composed from a `foreach` loop variable) is REPORTED at any destination where a check
    would otherwise have applied to it, rather than dropped.

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
#
# A target-name position accepts `${...}` as well as a bare identifier: see the module docstring — a
# variable-named target is a real target, and matching literals only is how the macOS branches came to
# be invisible to every check below.
_NAME = r"[A-Za-z_$][\w${}]*"
_ACQUIRE = re.compile(r"\bcontext_acquire_cef\s*\(")
_STAGE_CALL = re.compile(rf"\bcontext_cef_stage_payload\s*\(\s*({_NAME})")
# COPY_FILES(<target> <files> <src dir> <dst dir>) — possibly wrapped across lines.
_COPY_FILES = re.compile(rf"\bCOPY_FILES\s*\(\s*({_NAME})([^)]*)\)", re.DOTALL)
_CEF_EXE = re.compile(rf"\bSET_EXECUTABLE_TARGET_PROPERTIES\s*\(\s*({_NAME})\s*\)")
_ADD_DEPENDENCIES = re.compile(rf"\badd_dependencies\s*\(\s*({_NAME})([^)]*)\)", re.DOTALL)

# `set(<var> <value>)` — only the single-value form is resolvable, which is all a target name ever is.
_SET_CALL = re.compile(r"^[ \t]*set\s*\(\s*([A-Za-z_]\w*)\s+([^\n]*?)\s*\)[ \t]*$", re.MULTILINE)

# --- check 5: the hand-written CEF-hosting roster --------------------------------------------------
# The variable name is the contract between this lint and src/editor/shell/CMakeLists.txt. It is a
# MULTI-VALUE set() spanning many lines, so `_SET_CALL` above (deliberately single-value, because a
# target name is never a list) cannot read it and this pattern exists alongside it.
_ROSTER_VAR = "_ctx_cef_shell_executables"
# `\b\s*` and NOT `\s+`: an EMPTIED roster is spelled `set(_ctx_cef_shell_executables)` with no
# whitespace at all, and requiring some made that exact shape INVISIBLE to this check -- so the one
# mutation the "is EMPTY" finding exists for would have passed vacuously. Found by writing that case.
# The `\b` is what still keeps a longer identifier (`..._executables_old`) from matching.
_ROSTER_SET = re.compile(rf"\bset\s*\(\s*{_ROSTER_VAR}\b\s*([^)]*)\)", re.DOTALL)

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

# --- the platform axis ----------------------------------------------------------------------------
# The three v1 desktop platforms, as the set every call site is attributed to.
WINDOWS, LINUX, MAC = "windows", "linux", "mac"
ALL_PLATFORMS = frozenset({WINDOWS, LINUX, MAC})

# How a CMake `if()` variable constrains that set. Only the platform variables this repo's build files
# actually use are listed; anything else is a non-platform atom (see `_eval_condition`).
_PLATFORM_ATOMS = {
    "OS_WINDOWS": frozenset({WINDOWS}),
    "WIN32": frozenset({WINDOWS}),
    "MSVC": frozenset({WINDOWS}),
    "OS_LINUX": frozenset({LINUX}),
    "OS_MAC": frozenset({MAC}),
    "OS_MACOSX": frozenset({MAC}),
    "APPLE": frozenset({MAC}),
    "UNIX": frozenset({LINUX, MAC}),
}

# CMake `if()` operators that consume operands, so an unrecognised comparison is swallowed whole rather
# than leaving its right-hand side to be read as another atom.
_UNARY_TESTS = frozenset(
    {"EXISTS", "COMMAND", "DEFINED", "POLICY", "TARGET", "TEST", "IS_DIRECTORY",
     "IS_SYMLINK", "IS_ABSOLUTE", "IS_READABLE", "IS_WRITABLE", "IS_EXECUTABLE"}
)
_BINARY_TESTS = frozenset(
    {"EQUAL", "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL", "STREQUAL", "STRLESS",
     "STRGREATER", "STRLESS_EQUAL", "STRGREATER_EQUAL", "MATCHES", "IN_LIST", "PATH_EQUAL",
     "VERSION_EQUAL", "VERSION_LESS", "VERSION_GREATER", "VERSION_LESS_EQUAL",
     "VERSION_GREATER_EQUAL", "IS_NEWER_THAN"}
)

_IF_LINE = re.compile(r"^[ \t]*(if|elseif|else|endif)\s*\((.*)\)[ \t]*$", re.IGNORECASE)
_CONDITION_TOKEN = re.compile(r'\(|\)|"[^"]*"|[^\s()]+')


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


# --- name resolution ------------------------------------------------------------------------------


def set_variables(text: str) -> dict[str, str]:
    """File-local `set(<var> <single value>)` assignments, for resolving `${var}` target names.

    Deliberately shallow: only the one-value form, and a later assignment wins. That is exactly the
    shape a target/app-path name is written in, and anything more (list values, PARENT_SCOPE, CACHE)
    cannot name a single target anyway.
    """
    values: dict[str, str] = {}
    for match in _SET_CALL.finditer(text):
        raw = match.group(2).strip()
        if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
            raw = raw[1:-1]
        elif " " in raw or "\t" in raw:
            continue  # a list, or a set() with options — not a plain single value
        values[match.group(1)] = raw
    return values


def resolve(name: str, values: dict[str, str]) -> str:
    """Substitute `${var}` from `values`, repeatedly, until nothing more resolves.

    A name that still contains `${` afterwards is UNRESOLVED — typically composed from a `foreach` loop
    variable (`${_cef_helper_target}${_target_suffix}`). Callers must treat that case explicitly rather
    than silently skipping it; that silence is what hid the macOS targets.
    """
    for _ in range(8):
        replaced = re.sub(
            r"\$\{([A-Za-z_]\w*)\}",
            lambda m: values.get(m.group(1), m.group(0)),
            name,
        )
        if replaced == name:
            break
        name = replaced
    return name


def is_unresolved(name: str) -> bool:
    return "${" in name


# --- platform attribution -------------------------------------------------------------------------


def _tokenize_condition(condition: str) -> list[str]:
    return _CONDITION_TOKEN.findall(condition)


def _eval_condition(condition: str) -> frozenset[str] | None:
    """The platform set on which an `if()` condition CAN hold, or None when it names no platform.

    None means "this condition says nothing about the platform axis", and the caller then keeps the
    enclosing set unchanged — which is what makes this addition behaviour-preserving for every
    pre-existing non-platform guard in the tree (`if(NOT CONTEXT_BUILD_GUI_CEF)`,
    `if(NOT TARGET libcef_lib)`, ...).

    Non-platform atoms inside a condition that DOES name a platform are taken as TRUE, so
    `if(CONTEXT_BUILD_GUI_CEF AND (OS_WINDOWS OR OS_LINUX))` yields {windows, linux}: the question
    being asked is "which platforms could enter this branch", not "is it entered".
    """
    tokens = _tokenize_condition(condition)
    if not any(t.upper() in _PLATFORM_ATOMS for t in tokens):
        return None

    pos = 0

    def peek() -> str | None:
        return tokens[pos] if pos < len(tokens) else None

    def take() -> str | None:
        nonlocal pos
        token = peek()
        if token is not None:
            pos += 1
        return token

    def parse_or() -> frozenset[str]:
        result = parse_and()
        while peek() is not None and peek().upper() == "OR":
            take()
            result = result | parse_and()
        return result

    def parse_and() -> frozenset[str]:
        result = parse_not()
        while peek() is not None and peek().upper() == "AND":
            take()
            result = result & parse_not()
        return result

    def parse_not() -> frozenset[str]:
        token = peek()
        if token is not None and token.upper() == "NOT":
            take()
            return ALL_PLATFORMS - parse_not()
        if token == "(":
            take()
            inner = parse_or()
            if peek() == ")":
                take()
            return inner
        return parse_atom()

    def parse_atom() -> frozenset[str]:
        token = take()
        if token is None:
            return ALL_PLATFORMS
        upper = token.upper()
        if upper in _UNARY_TESTS:
            take()  # its single operand
            return ALL_PLATFORMS
        # A binary comparison: <lhs> OP <rhs>. `token` was the lhs.
        nxt = peek()
        if nxt is not None and nxt.upper() in _BINARY_TESTS:
            take()
            take()
            return ALL_PLATFORMS
        return _PLATFORM_ATOMS.get(upper, ALL_PLATFORMS)

    return parse_or()


def platform_map(text: str) -> list[tuple[int, frozenset[str]]]:
    """(line start offset, platform set) for every line of comment-stripped CMake text.

    Only `if`/`elseif` conditions narrow the set. `else()` deliberately does NOT: inverting a branch
    whose condition mixes platform and non-platform atoms is not sound, and being over-broad here can
    only ever ADD a requirement (never hide one), which is the safe direction for a lint. Every branch
    in this repo that needs a platform-specific else spells it `elseif(OS_MAC)`.
    """
    offsets: list[tuple[int, frozenset[str]]] = []
    # Each entry: (mask inside the current branch, mask enclosing the whole if-chain).
    stack: list[tuple[frozenset[str], frozenset[str]]] = []
    offset = 0
    for line in text.split("\n"):
        current = stack[-1][0] if stack else ALL_PLATFORMS
        match = _IF_LINE.match(line)
        if match is not None:
            keyword = match.group(1).lower()
            condition = match.group(2)
            if keyword == "if":
                narrowed = _eval_condition(condition)
                stack.append(((current & narrowed) if narrowed else current, current))
                current = stack[-1][0]
            elif keyword == "elseif" and stack:
                enclosing = stack[-1][1]
                narrowed = _eval_condition(condition)
                stack[-1] = (((enclosing & narrowed) if narrowed else enclosing), enclosing)
                current = stack[-1][0]
            elif keyword == "else" and stack:
                stack[-1] = (stack[-1][1], stack[-1][1])
                current = stack[-1][0]
            elif keyword == "endif" and stack:
                stack.pop()
                current = stack[-1][0] if stack else ALL_PLATFORMS
        offsets.append((offset, current))
        offset += len(line) + 1
    return offsets


def platform_at(offsets: list[tuple[int, frozenset[str]]], position: int) -> frozenset[str]:
    """The platform set of the line containing `position` (the line a call site STARTS on)."""
    found = ALL_PLATFORMS
    for start, mask in offsets:
        if start > position:
            break
        found = mask
    return found


# --- balanced-paren call extraction ---------------------------------------------------------------


def calls(text: str, command: str):
    """Yield (start offset, argument text) for each `<command>(...)`, honouring nested parens.

    Used where a regex `[^)]*` body would be unsafe: `add_custom_command(TARGET ...)` bodies are long
    and may legitimately contain parentheses.
    """
    pattern = re.compile(rf"\b{re.escape(command)}\s*\(", re.IGNORECASE)
    for match in pattern.finditer(text):
        depth = 1
        i = match.end()
        in_quotes = False
        while i < len(text) and depth > 0:
            ch = text[i]
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_quotes = not in_quotes
            elif not in_quotes and ch == "(":
                depth += 1
            elif not in_quotes and ch == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield match.start(), text[match.end() : i]


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


def _platforms(names: frozenset[str]) -> str:
    return "/".join(sorted(names)) if names else "no platform"


def _colliding_writers(platforms_by_target: dict[str, frozenset[str]]) -> list[str]:
    """The target names that write ONE destination on a platform they SHARE, sorted.

    Empty means no two writers' platform sets intersect -- which IS the single-writer invariant, and
    the reason a macOS-only writer never collides with a Windows/Linux one. Checks 1 and 4 differ only
    in what a "destination" is (a ${CEF_TARGET_OUT_DIR} vs an .app's Contents/Frameworks), so the
    pairwise comparison itself lives here once instead of being written twice.
    """
    items = sorted(platforms_by_target.items())
    involved: set[str] = set()
    for index, (name, platforms) in enumerate(items):
        for other, other_platforms in items[index + 1 :]:
            if platforms & other_platforms:
                involved.update((name, other))
    return sorted(involved)


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

    # check 5's subject: the hand-written roster, and where it was declared.
    rosters: list[tuple[Path, list[str]]] = []

    # Per staging destination: its writers, its stage target, and its CEF executables. Each entry
    # carries the union of the platform sets its call sites were attributed to.
    writers: dict[Path, dict[str, tuple[Path, frozenset[str]]]] = {}
    stages: dict[Path, tuple[str, Path, frozenset[str]]] = {}
    exes: dict[Path, dict[str, tuple[Path, frozenset[str]]]] = {}
    deps: dict[str, dict[str, frozenset[str]]] = {}
    # macOS: app bundle path -> {writing target: (kind, directory, platforms)}.
    bundle_writers: dict[str, dict[str, tuple[str, Path, frozenset[str]]]] = {}

    def note(store: dict, key: str, directory: Path, platforms: frozenset[str]) -> None:
        previous = store.get(key)
        store[key] = (directory, platforms | previous[1]) if previous else (directory, platforms)

    for directory, text in texts.items():
        root = stage_root(directory, acquire_dirs)
        offsets = platform_map(text)
        values = set_variables(text)

        def at(position: int) -> frozenset[str]:
            return platform_at(offsets, position)

        for match in _COPY_FILES.finditer(text):
            if "CEF_TARGET_OUT_DIR" not in match.group(2):
                continue  # a copy to some other destination is none of this lint's business
            target = resolve(match.group(1), values)
            if root is None:
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"COPY_FILES({target} ... ${{CEF_TARGET_OUT_DIR}}) but no context_acquire_cef() "
                    f"in this directory or any ancestor -- the destination is undefined here."
                )
                continue
            note(writers.setdefault(root, {}), target, directory, at(match.start()))

        for match in _STAGE_CALL.finditer(text):
            stage_target = resolve(match.group(1), values)
            platforms = at(match.start())
            if root is None:
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"context_cef_stage_payload({stage_target}) needs context_acquire_cef() in the "
                    f"same directory scope (it supplies CEF_TARGET_OUT_DIR and the payload lists)."
                )
                continue
            if root in stages:
                findings.append(
                    f"{root.relative_to(repo_root).as_posix()}: two stage targets "
                    f"({stages[root][0]}, {stage_target}) serve one CEF staging destination."
                )
                continue
            stages[root] = (stage_target, directory, platforms)
            note(writers.setdefault(root, {}), stage_target, directory, platforms)

        if root is not None:
            for match in _CEF_EXE.finditer(text):
                note(
                    exes.setdefault(root, {}),
                    resolve(match.group(1), values),
                    directory,
                    at(match.start()),
                )

        for match in _ADD_DEPENDENCIES.finditer(text):
            platforms = at(match.start())
            consumer = deps.setdefault(resolve(match.group(1), values), {})
            for dependency in match.group(2).split():
                resolved = resolve(dependency, values)
                consumer[resolved] = consumer.get(resolved, frozenset()) | platforms

        for match in _ROSTER_SET.finditer(text):
            rosters.append((directory, match.group(1).split()))

        # --- macOS app-bundle payload writers (check 4) --------------------------------------------
        for start, args in calls(text, "COPY_MAC_FRAMEWORK"):
            parts = [a.strip('"') for a in args.split()]
            if len(parts) < 3:
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"COPY_MAC_FRAMEWORK takes (<target> <binary dir> <app path>); got {args!r}."
                )
                continue
            target = resolve(parts[0], values)
            app = resolve(parts[-1], values).rstrip("/")
            store = bundle_writers.setdefault(app, {})
            previous = store.get(target)
            merged = at(start) | (previous[2] if previous else frozenset())
            store[target] = ("framework", directory, merged)
            if not app.endswith(f"/{target}.app"):
                findings.append(
                    f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt: "
                    f"COPY_MAC_FRAMEWORK({target} ... {app}) embeds the framework into a bundle that "
                    f"is not {target}'s own .app. The embed must be attached to the target that OWNS "
                    f"the bundle -- a helper or a sibling doing it is a second writer into someone "
                    f"else's Contents/Frameworks (the macOS form of issue #360)."
                )

        for start, args in calls(text, "add_custom_command"):
            head = args.lstrip()
            if not head.upper().startswith("TARGET"):
                continue
            target_match = re.match(rf"TARGET\s+({_NAME})", head, re.IGNORECASE)
            if target_match is None:
                continue
            for dest in re.findall(r'"([^"]*?)/Contents/Frameworks[^"]*"', args):
                app = resolve(dest, values).rstrip("/")
                target = resolve(target_match.group(1), values)
                store = bundle_writers.setdefault(app, {})
                previous = store.get(target)
                merged = at(start) | (previous[2] if previous else frozenset())
                store[target] = (previous[0] if previous else "helper-copy", directory, merged)

    # --- check 1: one writer per destination ------------------------------------------------------
    for root, by_target in sorted(writers.items()):
        involved = _colliding_writers({n: entry[1] for n, entry in by_target.items()})
        if not involved:
            continue
        listed = ", ".join(
            f"{name} ({by_target[name][0].relative_to(repo_root).as_posix()}, "
            f"{_platforms(by_target[name][1])})"
            for name in involved
        )
        findings.append(
            f"{root.relative_to(repo_root).as_posix()}: {len(involved)} targets stage the CEF "
            f"payload into the SAME ${{CEF_TARGET_OUT_DIR}} on one platform -- {listed}. Concurrent "
            f"POST_BUILD copies into one directory are issue #360 (a Windows sharing violation "
            f"whenever ninja links two of them together). Stage once via "
            f"context_cef_stage_payload() and give every consumer add_dependencies(<exe> "
            f"<stage target>)."
        )

    # --- check 2: every CEF executable depends on its destination's stage target -------------------
    # ONLY on the platforms where both the executable and the stage target exist. macOS builds no stage
    # target at all (COPY_MAC_FRAMEWORK embeds the payload per .app), so a macOS-only CEF executable
    # must NOT take the edge -- demanding it is what redded all three legs before e12c-1.
    for root, (stage_target, _stage_dir, stage_platforms) in sorted(stages.items()):
        for exe, (directory, exe_platforms) in sorted(exes.get(root, {}).items()):
            if exe == stage_target:
                continue
            shared = exe_platforms & stage_platforms
            if not shared:
                continue
            where = directory.relative_to(repo_root).as_posix()
            if is_unresolved(exe):
                findings.append(
                    f"{where}/CMakeLists.txt: CEF executable `{exe}` has a target name this lint "
                    f"cannot resolve, at the staging destination served by {stage_target} on "
                    f"{_platforms(shared)}. Its stage dependency is therefore UNVERIFIED. Name the "
                    f"target literally, or resolve it through a plain set(<var> <name>) in the same "
                    f"file."
                )
                continue
            covered = deps.get(exe, {}).get(stage_target, frozenset())
            if not shared <= covered:
                findings.append(
                    f"{where}/CMakeLists.txt: CEF executable "
                    f"{exe} runs from the staging destination served by {stage_target} on "
                    f"{_platforms(shared)} but does not depend on it there. Add "
                    f"add_dependencies({exe} {stage_target}) -- do NOT give it its own COPY_FILES() "
                    f"POST_BUILD copy (issue #360)."
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

    # --- check 4: one writer per macOS .app bundle payload ----------------------------------------
    for app, by_target in sorted(bundle_writers.items()):
        involved = _colliding_writers({n: entry[2] for n, entry in by_target.items()})
        if involved:
            # The platform OVERLAP is what decided this finding, so name it, exactly as check 1's
            # sibling message does -- "on one platform" without saying which is not actionable.
            listed = ", ".join(
                f"{name} ({by_target[name][0]}, "
                f"{by_target[name][1].relative_to(repo_root).as_posix()}, "
                f"{_platforms(by_target[name][2])})"
                for name in involved
            )
            findings.append(
                f"{app}: {len(involved)} targets write into this bundle's Contents/Frameworks on one "
                f"platform -- {listed}. An .app's payload has ONE owning target, so its framework "
                f"embed and its helper-bundle copies all serialise inside that target's POST_BUILD "
                f"chain; spread across targets they are the macOS form of issue #360's concurrent "
                f"copies into one directory."
            )
        kinds = {kind for kind, _dir, _platforms in by_target.values()}
        if "framework" not in kinds:
            some_dir = sorted(by_target.values())[0][1]
            findings.append(
                f"{some_dir.relative_to(repo_root).as_posix()}/CMakeLists.txt: {app} receives helper "
                f"bundles but no COPY_MAC_FRAMEWORK. A macOS app bundle with no embedded Chromium "
                f"Embedded Framework cannot boot at all -- CefScopedLibraryLoader finds nothing to "
                f"load -- and the failure is a runtime one, on one OS."
            )

    # --- check 5: the hand-written roster equals the literal stage-consumer set --------------------
    findings.extend(roster_findings(repo_root, rosters, stages, deps))

    return findings, len(lists)


def roster_findings(
    repo_root: Path,
    rosters: list[tuple[Path, list[str]]],
    stages: dict[Path, tuple[str, Path, frozenset[str]]],
    deps: dict[str, dict[str, frozenset[str]]],
) -> list[str]:
    """Check 5 -- see the module docstring. The roster and the stage-edge consumers must be EQUAL."""
    if not rosters:
        # NO ROSTER, NO CHECK -- and the anti-vacuity guard for that lives one tier out, deliberately.
        # This lint is generic over trees and its own pytest builds a dozen synthetic ones with no
        # Shell, so a finding here would be noise on every one of them. But "no roster found" must not
        # be how a RENAMED or DELETED roster passes, so the claim that the LIVE tree declares exactly
        # one non-empty `_ctx_cef_shell_executables` is asserted in the live-tree half of
        # tools/tests/test_cef_staging.py -- itself a blocking gate (the python-tests job), and the
        # right home for a claim about THIS repository rather than about any repository.
        return []
    if len(rosters) > 1:
        where = ", ".join(
            f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt" for directory, _ in rosters
        )
        return [
            f"{where}: {len(rosters)} declarations of {_ROSTER_VAR}. It is THE ONE roster -- two of "
            f"them is how the pre-e12c-2 duplicate lists drifted apart."
        ]

    directory, roster = rosters[0]
    where = f"{directory.relative_to(repo_root).as_posix()}/CMakeLists.txt"
    findings: list[str] = []

    unresolved = sorted(name for name in roster if is_unresolved(name))
    if unresolved:
        findings.append(
            f"{where}: {_ROSTER_VAR} contains name(s) this lint cannot resolve ({', '.join(unresolved)}). "
            f"Both audits iterate the list literally, so name every member literally too."
        )
    if not roster:
        return findings + [
            f"{where}: {_ROSTER_VAR} is EMPTY. Both configure-time CEF audits iterate it, so an empty "
            f"roster audits nothing while every check over it passes -- the vacuous direction."
        ]

    stage_targets = {stage_target for stage_target, _dir, _platforms in stages.values()}
    if not stage_targets:
        return findings + [
            f"{where}: {_ROSTER_VAR} names {len(roster)} target(s) but no context_cef_stage_payload() "
            f"stage target exists under src/ to cross-check them against."
        ]

    consumers = {
        consumer
        for consumer, dependencies in deps.items()
        if stage_targets & set(dependencies)
    }

    for listed in sorted(set(roster)):
        if listed not in consumers:
            findings.append(
                f"{where}: {_ROSTER_VAR} names {listed}, but no "
                f"`add_dependencies({listed} <stage target>)` edge exists in the sources. Either the "
                f"target was renamed/removed -- update the roster -- or it lost the issue-#360 "
                f"staging edge, which check 2 covers only for executables it can SEE."
            )
    for consumer in sorted(consumers - set(roster)):
        findings.append(
            f"{where}: {consumer} takes the CEF stage dependency but is MISSING from {_ROSTER_VAR}, "
            f"so BOTH configure-time audits skip it -- silently UNDER-AUDITED rather than caught. "
            f"That is exactly how context_editor_shell_uimirror_smoke and "
            f"context_editor_shell_iframe_smoke went unchecked for two tasks. Add it to the roster."
        )
    return findings


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
