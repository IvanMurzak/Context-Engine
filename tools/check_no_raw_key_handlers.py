#!/usr/bin/env python3
"""Fail on any RAW DOM key handler in editor-core that bypasses the keymap/command path (M9 e07d).

Design 05 §6: "no raw key handlers anywhere (enforced by review + a lint in T1)". Every keyboard
capability in the editor MUST flow through the ONE command registry (e07b) and the keymap (e07c) — a
keystroke resolves to a command id and the registry executes it. An ad-hoc `keydown`/`keyup`/`keypress`
listener (or an `onkeydown=` property handler) that implements a shortcut of its own is exactly the
drift this lint exists to stop: it would bypass the when-context resolution, the user's keybindings
override, and the command surface an agent/CI drives.

This is a SOURCE scan (like tools/check_webui_assets.py), so it runs on every default `build` leg with
no browser — it is registered as the `webui-no-raw-key-handlers` ctest in the `webui-*` family.

The rule, and its ONE escape hatch. Any of these patterns in an editor-core source is a FINDING:

    addEventListener("keydown"|"keyup"|"keypress", ...)     (single or double quotes)
    .onkeydown = / .onkeyup = / .onkeypress =

UNLESS the attach site is explicitly justified with a marker comment `key-handler-ok:` on the SAME
line or within the few lines just above it. That marker is the "enforced by review" half made
mechanical: a legitimately-on-the-command-path handler (the hydration runtime's Enter/Space ARIA
ACTIVATION of a focusable node, or the palette's own list navigation while it is open) carries the
marker with a one-line reason, and any NEW unmarked key handler fails the build. Test sources are
exempt (they are not shipped and may synthesize events freely).

Exit codes: 0 = clean, 1 = at least one unjustified raw key handler, 2 = a configuration error.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The key-event handler attach patterns. Whitespace-tolerant so a reformat cannot slip one past.
_ADD_LISTENER = re.compile(
    r"""addEventListener\s*\(\s*['"](?:keydown|keyup|keypress)['"]"""
)
_ON_PROPERTY = re.compile(r"""\.on(?:keydown|keyup|keypress)\s*=""")

# The review-justification marker that whitelists an on-the-command-path handler.
_MARKER = "key-handler-ok:"

# How many lines ABOVE the attach site may carry the marker (a comment block just above the call).
_MARKER_LOOKBACK = 3

# TypeScript source extensions editor-core uses.
_TS_EXTENSIONS = {".ts", ".tsx", ".mts", ".cts"}


def _is_test_source(path: Path) -> bool:
    """Test sources are exempt — they are not shipped and may synthesize key events freely."""
    return any(part == "test" or part == "tests" for part in path.parts)


def _scan_file(path: Path) -> list[tuple[int, str]]:
    """Return (line_number, line_text) for every unjustified key-handler attach in `path`."""
    findings: list[tuple[int, str]] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if not (_ADD_LISTENER.search(line) or _ON_PROPERTY.search(line)):
            continue
        # Justified when the marker is on this line or within the small look-back window above it.
        window = lines[max(0, index - _MARKER_LOOKBACK) : index + 1]
        if any(_MARKER in w for w in window):
            continue
        findings.append((index + 1, line.strip()))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sources",
        required=True,
        help="the editor-core TS source directory to scan (src/editor/webui/core/src)",
    )
    args = parser.parse_args(argv)

    root = Path(args.sources)
    if not root.is_dir():
        print(f"[no-raw-key-handlers] ERROR: not a directory: {root}", file=sys.stderr)
        return 2

    sources = sorted(
        p
        for p in root.rglob("*")
        if p.is_file() and p.suffix in _TS_EXTENSIONS and not _is_test_source(p)
    )
    if not sources:
        print(f"[no-raw-key-handlers] ERROR: no TS sources under {root}", file=sys.stderr)
        return 2

    total = 0
    for path in sources:
        for line_no, text in _scan_file(path):
            total += 1
            print(
                f"[no-raw-key-handlers] FINDING: {path.as_posix()}:{line_no}: raw key handler "
                f"without a `{_MARKER}` justification: {text}",
                file=sys.stderr,
            )

    if total:
        print(
            f"[no-raw-key-handlers] FAILED: {total} unjustified raw key handler(s). Route the "
            f"keystroke through the keymap + command registry (05 §6), or — for an on-the-command-"
            f"path handler — add a `{_MARKER} <reason>` marker at the attach site.",
            file=sys.stderr,
        )
        return 1

    print(f"[no-raw-key-handlers] OK: scanned {len(sources)} editor-core source(s); no raw key handlers.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
