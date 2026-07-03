"""Shared test scaffolding for the tools/ CI gate scripts.

The gates are plain scripts (no package), invoked as files — ``python3 tools/<gate>.py``
— where the script-dir sys.path entry resolves their sibling import (_ci_common).
`load_tool` mirrors that arrangement: it puts tools/ on sys.path and imports a gate
by file path, exactly as its file-based invocation would.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))


def load_tool(name: str):
    """Import tools/<name>.py as a module named <name> and return it."""
    spec = importlib.util.spec_from_file_location(name, TOOLS_DIR / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module
