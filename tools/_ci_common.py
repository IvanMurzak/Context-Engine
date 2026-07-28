"""Shared helpers for the tools/ CI gate scripts.

The gate scripts are invoked as files (``python3 tools/<gate>.py``), so this sibling
module resolves via the script-dir sys.path entry; tools/tests/conftest.py mirrors
that arrangement for the test suite.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def load_json_or_exit(path: Path, *, tag: str) -> dict:
    """Load a JSON file, or exit(2) with a uniform ``[tag] ERROR: ...`` on stderr.

    Exit code 2 is the gates' shared configuration-error code: an unreadable or
    malformed input is a clean, loud config failure — never a traceback.
    """
    try:
        with path.open(encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[{tag}] ERROR: cannot read {path}: {exc}", file=sys.stderr)
        sys.exit(2)


def strip_comments(text: str) -> str:
    """Remove CMake `#` comments, honouring double-quoted strings (which may contain `#`).

    Shared because every gate that reads CMake sources needs it and must agree on the answer: a
    comment-BLIND scan reads this repository's own prose about CMake as if it were CMake. That is not
    hypothetical here — `src/editor/shell/cef/CMakeLists.txt` documents its `DISABLED TRUE` history in
    comments, and both `add_test(` and `DISABLED TRUE` appear inside them.

    ⚠ THIS IS THE CMake `#` VARIANT, and that scope is deliberate. Several other gates under tools/
    carry their OWN `strip_comments` for a DIFFERENT language — check_session_ownership.py's knows
    C/C++ comments, as its own test suite records — so they are NOT duplicates of this one and must not
    be collapsed into it. Only a gate that scans CMakeLists.txt should import this.
    """
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
