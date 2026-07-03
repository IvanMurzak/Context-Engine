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
