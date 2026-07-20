#!/usr/bin/env python3
"""Verify the built editor-core asset set (M9 e05a).

The asset set is what the Shell will serve over the ``context-editor://app/`` scheme (design 04
section 1): the esbuild-produced ``editor-core.js`` bundle plus the SHA-pinned dockview-core
distribution files staged beside it. This script is the ``webui-assets`` ctest's body — it asserts
the three properties a green build must actually have, none of which an exit code from esbuild
proves on its own:

  1. **The bundle is real.** Present, non-empty, and carrying the entry module's exported symbols —
     an empty or truncated output file otherwise "succeeds" silently.
  2. **No Node at runtime** (the standing repo convention, 04 section 1). The asset must not
     reference a Node builtin specifier (``node:*``) and must not carry a ``#!/usr/bin/env node``
     shebang. Deliberately narrow: these two markers are unambiguous Node-runtime tells that will
     never legitimately appear in a browser asset, so the check stays valid once e05c bundles
     dockview's UMD (whose CommonJS ``require`` shim is NOT a Node dependency and must not fail
     this gate).
  3. **The supply chain reached the asset dir intact.** Every dockview member staged beside the
     bundle is re-hashed against ``tools/dockview-toolchain.json`` — so a file swapped AFTER
     ``fetch_dockview.py`` verified it (a stale build dir, a partial copy, local tampering) is
     still caught at test time, not just at fetch time. This is the verify-before-use posture
     carried one step further, to verify-at-use.

Exit codes (mirrors tools/check_licenses.py / tools/fetch_dockview.py):
  * 0 — every asset present and verified.
  * 1 — an assertion FAILED (missing/empty/altered asset, Node reference).
  * 2 — configuration / usage error (unreadable manifest or asset dir).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "dockview-toolchain.json"

# Symbols the e05a entry module (src/editor/webui/core/src/index.ts) exports. Their presence proves
# the bundle carries the real entry, not an empty or unrelated file.
REQUIRED_MARKERS = ("editorCoreInfo", "isRpcMethod", "isEventTopic", "isRetriable")

# Unambiguous Node-runtime tells (see the module docstring, check 2).
NODE_MARKERS = ('"node:', "'node:", "#!/usr/bin/env node")

_CHUNK = 1 << 20


class CheckError(Exception):
    """Configuration / usage problem (maps to exit 2)."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(_CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def check_bundle(bundle: Path) -> list[str]:
    """Return a list of failure strings for the JS bundle (empty == pass)."""
    failures: list[str] = []
    if not bundle.is_file():
        return [f"bundle missing: {bundle}"]
    text = bundle.read_text(encoding="utf-8", errors="replace")
    if not text.strip():
        return [f"bundle is empty: {bundle}"]
    for marker in REQUIRED_MARKERS:
        if marker not in text:
            failures.append(
                f"bundle {bundle.name} does not export '{marker}' — the entry module did not "
                f"make it into the bundle")
    for marker in NODE_MARKERS:
        if marker in text:
            failures.append(
                f"bundle {bundle.name} references Node ({marker!r}) — editor-core assets must run "
                f"in the browser zone with no Node at runtime (design 04 section 1)")
    return failures


def check_dockview(asset_dir: Path, manifest: dict) -> list[str]:
    """Re-verify every staged dockview member against its pin. Empty list == pass."""
    failures: list[str] = []
    members = manifest.get("members")
    if not isinstance(members, dict) or not members:
        raise CheckError("dockview manifest has no 'members' table")
    for name, spec in sorted(members.items()):
        expected = spec.get("sha256")
        if not expected:
            raise CheckError(f"dockview manifest member '{name}' has no 'sha256' pin")
        staged = asset_dir / name
        if not staged.is_file():
            failures.append(f"dockview asset missing from the asset dir: {staged}")
            continue
        actual = _sha256(staged)
        if actual != expected:
            failures.append(
                f"dockview asset '{name}' FAILED its pin: expected {expected}, got {actual} — the "
                f"staged file changed after fetch_dockview.py verified it")
    return failures


def run(asset_dir: Path, manifest_path: Path, bundle_name: str) -> int:
    if not asset_dir.is_dir():
        raise CheckError(f"asset dir does not exist: {asset_dir}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CheckError(f"cannot read manifest {manifest_path}: {exc}") from exc

    failures = check_bundle(asset_dir / bundle_name)
    failures.extend(check_dockview(asset_dir, manifest))

    if failures:
        for failure in failures:
            print(f"[check_webui_assets] FAIL: {failure}", file=sys.stderr)
        return 1

    member_count = len(manifest.get("members", {}))
    print(f"[check_webui_assets] OK: {bundle_name} + {member_count} pinned dockview asset(s) "
          f"verified in {asset_dir}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify the built editor-core asset set.")
    parser.add_argument("--asset-dir", type=Path, required=True,
                        help="the built asset dir (bundle + staged dockview files)")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="dockview pin manifest (default: tools/dockview-toolchain.json)")
    parser.add_argument("--bundle-name", default="editor-core.js",
                        help="bundle file name inside --asset-dir")
    args = parser.parse_args(argv)

    try:
        return run(args.asset_dir, args.manifest, args.bundle_name)
    except CheckError as exc:
        print(f"[check_webui_assets] ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
