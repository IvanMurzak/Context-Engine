#!/usr/bin/env python3
"""D10 boundary gate: the published client surface must not expose kernel internals.

The M9 target architecture rests on one structural claim (02 §1): the editor is an ORDINARY client
that talks only the public contract and "never links kernel internals". That claim is cheap to state
and easy to erode — one convenient `#include "context/editor/editorkernel/..."` in a public header
and the boundary is gone, with nothing failing.

This gate makes the claim enforceable. It checks two things against a REAL `cmake --install` tree:

  1. The installed include tree exposes ONLY the allowed modules, and every `#include` inside it
     resolves to a file that is also installed. A public header that includes a header we do not
     ship is a broken package for a consumer AND a boundary leak — the same defect, two symptoms.
  2. The out-of-tree consumer's own sources include nothing outside that surface.

Rule 1 is what makes the gate strong: it is transitive. A public header cannot quietly reach an
internal one via a chain of intermediate headers, because every hop must itself be installed, and
the installed set is the allowlist.

Exit codes: 0 = the boundary holds, 1 = a violation, 2 = a usage/config error (nothing to check).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# The ONLY module trees whose headers may be part of the published client surface. Widening this set
# widens the boundary: it is a deliberate, reviewable act, which is the point of listing it here AND
# in src/CMakeLists.txt's install(DIRECTORY) loop.
ALLOWED_MODULES = (
    "context/editor/client",   # the SDK itself
    "context/editor/bridge",   # transport + event-stream types its headers name
    "context/editor/contract", # envelope / json / handshake / registry
    "context/kernel",          # event_bus.h — bridge forwards kernel::LogEvent
)

# Defense in depth. Rule 1 already rejects anything not installed, but naming the internals makes a
# violation legible ("you reached into editorkernel") instead of merely "unresolved include", and it
# catches a future accidental install() of an internal module's headers by NAME.
FORBIDDEN_PREFIXES = (
    "context/editor/editorkernel/",
    "context/editor/filesync/",
    "context/editor/derivation/",
    "context/editor/compose/",
    "context/editor/merge/",
    "context/editor/migrate/",
    "context/editor/assetdb/",
    "context/editor/import/",
    "context/editor/schema/",
    "context/editor/serializer/",
    "context/editor/component/",
    "context/editor/kinds/",
    "context/editor/pack/",
    "context/editor/pkg/",
    "context/editor/build/",
    "context/editor/gui/",
    "context/editor/cef/",
    "context/editor/system/",
    "context/editor/schedule/",
    "context/editor/tilemap/",
    "context/runtime/",
    "context/packages/",
    "context/render/",
    "context/cli/",
    "context/common/",
    "context/testing/",
)

_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".cxx")


def find_include_root(install_prefix: Path) -> Path | None:
    """Locate <prefix>/versions/<semver>/include (the R-VER-004 side-by-side layout)."""
    direct = install_prefix / "include"
    if direct.is_dir():
        return direct
    versions = install_prefix / "versions"
    if versions.is_dir():
        for entry in sorted(versions.iterdir()):
            candidate = entry / "include"
            if candidate.is_dir():
                return candidate
    return None


def project_includes(text: str) -> list[str]:
    """Every `context/...` include in one file (system/stdlib includes are not our business)."""
    return [inc for inc in _INCLUDE_RE.findall(text) if inc.startswith("context/")]


def is_allowed_module(include: str) -> bool:
    return any(include.startswith(module + "/") for module in ALLOWED_MODULES)


def forbidden_prefix(include: str) -> str | None:
    for prefix in FORBIDDEN_PREFIXES:
        if include.startswith(prefix):
            return prefix
    return None


def check_file(path: Path, rel: str, include_root: Path, violations: list[dict]) -> int:
    """Check one file's includes. Returns the number of `context/...` includes examined."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - unreadable file is a config error, not a boundary bug
        violations.append({"file": rel, "include": "", "reason": f"unreadable: {exc}"})
        return 0

    includes = project_includes(text)
    for include in includes:
        prefix = forbidden_prefix(include)
        if prefix is not None:
            violations.append(
                {
                    "file": rel,
                    "include": include,
                    "reason": f"kernel-internal module '{prefix}' is not part of the published client surface",
                }
            )
            continue
        if not is_allowed_module(include):
            violations.append(
                {
                    "file": rel,
                    "include": include,
                    "reason": "outside the published modules " + ", ".join(ALLOWED_MODULES),
                }
            )
            continue
        if not (include_root / include).is_file():
            violations.append(
                {
                    "file": rel,
                    "include": include,
                    "reason": "resolves to no INSTALLED header (the package would not compile for a consumer)",
                }
            )
    return len(includes)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install-prefix", required=True, type=Path,
                        help="the `cmake --install` prefix holding versions/<semver>/include")
    parser.add_argument("--consumer", type=Path, action="append", default=[],
                        help="an out-of-tree consumer source dir to check (repeatable)")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable report")
    args = parser.parse_args(argv)

    include_root = find_include_root(args.install_prefix)
    if include_root is None:
        print(f"error: no installed include tree under {args.install_prefix}", file=sys.stderr)
        return 2

    violations: list[dict] = []
    headers_checked = 0
    includes_checked = 0

    # 1. The installed public surface, transitively.
    installed = sorted(p for p in include_root.rglob("*.h") if p.is_file())
    if not installed:
        print(f"error: installed include tree {include_root} contains no headers", file=sys.stderr)
        return 2
    for header in installed:
        rel = header.relative_to(include_root).as_posix()
        # An installed header outside the allowed modules is itself a leak — we shipped it.
        if not is_allowed_module(rel):
            violations.append(
                {
                    "file": rel,
                    "include": "",
                    "reason": "installed header is outside the published modules "
                              + ", ".join(ALLOWED_MODULES),
                }
            )
        headers_checked += 1
        includes_checked += check_file(header, rel, include_root, violations)

    # 2. The out-of-tree consumer(s).
    consumers_checked = 0
    for consumer in args.consumer:
        if not consumer.is_dir():
            print(f"error: consumer dir not found: {consumer}", file=sys.stderr)
            return 2
        for source in sorted(consumer.rglob("*")):
            if not source.is_file() or source.suffix not in SOURCE_SUFFIXES:
                continue
            rel = source.relative_to(consumer).as_posix()
            consumers_checked += 1
            includes_checked += check_file(source, f"<consumer>/{rel}", include_root, violations)

    report = {
        "includeRoot": str(include_root),
        "installedHeaders": headers_checked,
        "consumerSources": consumers_checked,
        "projectIncludesChecked": includes_checked,
        "allowedModules": list(ALLOWED_MODULES),
        "violations": violations,
        "ok": not violations,
    }

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"include-graph gate: root={include_root}")
        print(f"  installed headers : {headers_checked}")
        print(f"  consumer sources  : {consumers_checked}")
        print(f"  context/ includes : {includes_checked}")
        if violations:
            print(f"  VIOLATIONS ({len(violations)}):")
            for v in violations:
                where = v["file"]
                inc = f" -> {v['include']}" if v["include"] else ""
                print(f"    {where}{inc}: {v['reason']}")
        else:
            print("  OK — the published client surface exposes no kernel internals (D10).")

    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
