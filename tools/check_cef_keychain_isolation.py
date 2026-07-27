#!/usr/bin/env python3
"""Every CEF smoke must isolate Chromium's OSCrypt key from the MACHINE keychain (issue #437).

WHY A SOURCE SCAN. The rule is one line of code per smoke and its absence is INVISIBLE to every other
signal we have. A smoke that forgets it still builds, still links, still passes on any host whose
login keychain happens not to hold a `"<product> Safe Storage"` item yet — and then hangs FOREVER on
the next host, or on the same host after a rebuild, because macOS binds that item's ACL to the
CREATING executable's cdhash and securityd raises a modal SecurityAgent prompt no automated run can
answer. `CefShutdown()` waits on the ThreadPool shutdown event that the blocked BLOCK_SHUTDOWN
keychain task holds, so the process never exits; the smoke has printed its whole success verdict by
then. Measured on one binary a minute apart: 5/5 hangs without the isolation, 5/5 passes with it
(issue #437). That is a defect no runtime assertion can catch — a hung test asserts nothing — and it
is why the isolation is enforced from the SOURCES.

WHAT IS CHECKED. Two families, because there are two ways a CEF app in this tree reaches Chromium's
command line:

  1. **Shell smokes** — every source under `src/editor/shell/cef/src/` that CONSTRUCTS a
     `CefShellOptions` must set `use_mock_keychain` on it. They CANNOT pass the switch any other way:
     the Shell's `initialize()` builds `CefMainArgs(0, nullptr)` on POSIX, so a switch on the
     executable's own argv never reaches Chromium at all (verified — an argv-level A/B on these
     binaries changed nothing, which briefly read as "the fix does not work").
  2. **CEF apps that own their own `CefApp`** — every source under `src/editor/` that DEFINES
     `OnBeforeCommandLineProcessing` must name the `use-mock-keychain` switch.

Both subject sets are PREDICATES, not lists, and both had to be: a planting round caught a
filename-keyed rule 1 and a hardcoded rule 2 letting a new CEF file through GREEN.

Plus an ANTI-VACUITY half, because both prohibitions are trivially satisfied by a tree that deleted
the thing they protect: the Shell option must still EXIST in `cef_shell.h`, and `cef_shell.cpp` must
still both latch it and append the switch. Without that, deleting the option would make this gate
pass on a tree where no smoke isolates anything.

This is a REGISTRATION anchor, not a security boundary — its job is to make a forgotten line loud,
not to defeat someone routing around it. `tools/tests/test_check_cef_keychain_isolation.py` proves it
non-vacuous by planting each shape.

Exit 0 = pass. Exit 1 = a violation (each printed with its file and the exact line to add). Exit 2 = the scan
could not run (bad --repo-root, or an anchor file that moved), which is deliberately NOT a pass.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Where the Shell's smokes live. Every source here that CONSTRUCTS a CefShellOptions is a subject —
# keyed on the CONSTRUCTION, not on a `*_smoke.cpp` filename. FOUND BY PLANTING: a filename-keyed
# scan let a new CEF file in this very directory named anything else (e.g. `cef_shell_scenarios.cpp`)
# through GREEN, which is exactly the shape the next live scenario could take (e12c-2 fanned the
# EXISTING nine out to macOS and added no source; e12c-3 and every later T2 scenario will). The production
# editor also constructs these options (`src/editor/shell/app/editor_main.cpp`) and must NOT mock the
# keychain, which is why the subject set is this DIRECTORY rather than the type tree-wide.
SHELL_SMOKE_DIR = "src/editor/shell/cef/src"
SHELL_SOURCE_SUFFIXES = (".cpp", ".mm")

# A source is a Shell-smoke subject when it declares a CefShellOptions VALUE, however
# namespace-qualified, in EITHER spelling of construction — `CefShellOptions x;` or the equally
# idiomatic brace form `CefShellOptions x{};`. The `[;{]` is not cosmetic: keyed on `;` alone, a new
# scenario written `CefShellOptions cef_options{};` is not a subject at all and the gate reports it
# GREEN with no isolation — the same silent-exemption shape the planting round already caught twice
# (see the two `# FOUND BY PLANTING` cases in the pytest). Every site in the tree today uses the bare
# `;` form, so accepting `{` changes no present verdict; it closes a future hole.
# A by-reference parameter is not a construction, which is why `cef_shell.cpp` — whose
# `initialize(const CefShellOptions&)` merely receives one — is not a subject, and neither is a
# by-value RETURN type (`CefShellOptions make_cef_options(`), which is followed by `(`.
SHELL_OPTIONS_CONSTRUCTED = re.compile(r"\bCefShellOptions\s+[A-Za-z_]\w*\s*[;{]")

# Standalone CEF apps that own their own CefApp. The tuple is the KNOWN pair, asserted to exist so a
# rename is an exit-2 config error rather than a silent pass; the SCAN below is by predicate, over
# every source under `src/editor/` that DEFINES the hook. FOUND BY PLANTING: with only the tuple, a
# new standalone CEF app anywhere else was exempt and the gate stayed GREEN.
STANDALONE_APPS = (
    "src/editor/cef/src/cef_boot_smoke.cpp",
    "src/editor/gui/host/src/editor_host.cpp",
)
EDITOR_ROOT = "src/editor"
HOOK_SUFFIXES = (".cpp", ".mm", ".h", ".hpp")
COMMANDLINE_HOOK = re.compile(r"\bOnBeforeCommandLineProcessing\s*\(")
# Never scanned: build output and fetched third-party payloads.
SKIP_DIRS = {"build", "build-msvc-check", "node_modules", "vcpkg_installed", ".git", "_deps"}

# The two anchor files the option itself lives in.
OPTIONS_HEADER = "src/editor/shell/cef/include/context/editor/shell/cef/cef_shell.h"
SHELL_IMPL = "src/editor/shell/cef/src/cef_shell.cpp"

# `<anything>.use_mock_keychain = true` — the field assignment, however the options variable is
# named (the tree uses both `options.` and `cef_options.`) and across a line break.
SHELL_OPT_SET = re.compile(r"\.use_mock_keychain\s*=\s*true\s*;")

# `AppendSwitch("use-mock-keychain")` — the standalone form. The switch NAME is what matters, so the
# pattern keys on the literal rather than on the call shape.
SWITCH_LITERAL = re.compile(r'"use-mock-keychain"')

# Anti-vacuity anchors: the option must be DECLARED, LATCHED, and APPENDED.
OPTION_DECL = re.compile(r"\bbool\s+use_mock_keychain\s*=")
OPTION_LATCH = re.compile(r"g_use_mock_keychain\s*=\s*options\.use_mock_keychain\s*;")


def read(root: Path, rel: str) -> str:
    path = root / rel
    if not path.is_file():
        raise FileNotFoundError(f"anchor file {rel} not found under {root} — it moved or was renamed")
    return path.read_text(encoding="utf-8", errors="replace")


def iter_sources(root: Path, rel_dir: str, suffixes: tuple[str, ...]):
    """Every source under `rel_dir`, skipping build output and vendored payloads."""
    base = root / rel_dir
    if not base.is_dir():
        raise FileNotFoundError(f"{rel_dir} not found under {root}")
    for path in sorted(base.rglob("*")):
        if path.suffix not in suffixes or not path.is_file():
            continue
        if SKIP_DIRS.intersection(path.relative_to(base).parts):
            continue
        yield path.relative_to(root).as_posix(), path.read_text(encoding="utf-8", errors="replace")


def check(root: Path) -> list[str]:
    violations: list[str] = []

    # --- rule 1: every Shell smoke that CONSTRUCTS options must isolate the keychain --------------
    subjects = 0
    for rel, text in iter_sources(root, SHELL_SMOKE_DIR, SHELL_SOURCE_SUFFIXES):
        if SHELL_OPTIONS_CONSTRUCTED.search(text) is None:
            continue
        subjects += 1
        if SHELL_OPT_SET.search(text) is None:
            violations.append(
                f"{rel}: constructs a CefShellOptions but never sets `use_mock_keychain = true`. "
                f"Without it macOS blocks CefShutdown() forever on a keychain authorization prompt "
                f"(issue #437) and the smoke reports ***Timeout after printing its full success "
                f"output. Add `<options>.use_mock_keychain = true;` where it sets the rest of its "
                f"options.")
    if subjects == 0:
        raise FileNotFoundError(
            f"no source under {SHELL_SMOKE_DIR} constructs a CefShellOptions — the scan found no "
            f"subject at all, which is a moved-file failure, not a pass")

    # --- rule 2: every CEF app that owns a command-line hook must append the switch ---------------
    for rel in STANDALONE_APPS:
        read(root, rel)  # existence only; the predicate scan below owns the verdict
    hooks = 0
    for rel, text in iter_sources(root, EDITOR_ROOT, HOOK_SUFFIXES):
        if COMMANDLINE_HOOK.search(text) is None:
            continue
        hooks += 1
        if SWITCH_LITERAL.search(text) is None:
            violations.append(
                f"{rel}: defines OnBeforeCommandLineProcessing but never appends "
                f'"use-mock-keychain". Same consequence as above (issue #437): CefShutdown() never '
                f"returns on a host whose keychain already holds a Safe Storage item created by "
                f"another build. Add `command_line->AppendSwitch(\"use-mock-keychain\");` — "
                f"unconditionally for a test binary, or behind an opt-in flag as "
                f"{SHELL_IMPL} does. A PRODUCTION CEF app that must keep the real OS key store is a "
                f"deliberate review decision that edits this gate, not a silent exemption.")
    if hooks == 0:
        raise FileNotFoundError(
            f"no source under {EDITOR_ROOT} defines OnBeforeCommandLineProcessing — the scan found "
            f"no subject at all, which is a moved-file failure, not a pass")

    # --- anti-vacuity: the option this gate is about must still exist and still be wired ----------
    header = read(root, OPTIONS_HEADER)
    if OPTION_DECL.search(header) is None:
        violations.append(
            f"{OPTIONS_HEADER}: `CefShellOptions::use_mock_keychain` is GONE. Every check above "
            f"would then be satisfiable by a tree that isolates nothing, so its absence is a "
            f"violation in its own right (issue #437).")

    impl = read(root, SHELL_IMPL)
    if OPTION_LATCH.search(impl) is None:
        violations.append(
            f"{SHELL_IMPL}: nothing latches `options.use_mock_keychain` into `g_use_mock_keychain`. "
            f"The smokes would then set a field that reaches no command line. It MUST be latched "
            f"before CefInitialize, because OnBeforeCommandLineProcessing runs inside it.")
    if SWITCH_LITERAL.search(impl) is None:
        violations.append(
            f"{SHELL_IMPL}: the Shell never appends \"use-mock-keychain\" to the Chromium command "
            f"line, so `use_mock_keychain` is inert. Append it in "
            f"OnBeforeCommandLineProcessing under `g_use_mock_keychain`.")

    return violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".", help="the Context-Engine repository root")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve()
    if not root.is_dir():
        print(f"[cef-keychain] ERROR: --repo-root {root} is not a directory", file=sys.stderr)
        return 2
    try:
        violations = check(root)
    except FileNotFoundError as exc:
        print(f"[cef-keychain] ERROR: {exc}", file=sys.stderr)
        return 2

    if violations:
        for v in violations:
            print(f"[cef-keychain] FINDING: {v}", file=sys.stderr)
        print(f"[cef-keychain] FAIL: {len(violations)} CEF smoke(s) do not isolate the OSCrypt "
              f"keychain (issue #437)", file=sys.stderr)
        return 1

    print("[cef-keychain] PASS: every CEF smoke isolates the OSCrypt keychain, and the option is "
          "declared, latched and appended")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
