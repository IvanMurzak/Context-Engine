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

With ``--scheme-contract`` it additionally runs the M9 e05c gate over the SERVED DOCUMENT SET:

  4. **The document is CSP-clean.** ``index.html`` carries no inline ``<script>`` body and no
     ``style=`` attribute. The Shell serves ``script-src 'self'; style-src 'self'`` with no
     ``unsafe-inline`` (``app_csp_header()``), so an inline block is BLOCKED by the browser at
     runtime — i.e. the failure mode of forgetting this rule is a blank editor with a console
     message, not a build error. This turns it into a build error.
  5. **The scheme vocabulary does not drift across languages.** The scheme, editor-core's origin,
     the IPC endpoint and the injected query-function name are each declared in C++ (the Shell
     serves and routes with them) AND compiled into the TS bundle (editor-core calls with them).
     They are cross-checked here against the C++ headers, so a rename on one side reds CI instead
     of producing a silently unreachable bridge.
  6. **No ``file://`` reached the asset set** — a DoD line ("no ``file://`` fallback exists").

Exit codes (mirrors tools/check_licenses.py / tools/fetch_dockview.py):
  * 0 — every asset present and verified.
  * 1 — an assertion FAILED (missing/empty/altered asset, Node reference, CSP or scheme drift).
  * 2 — configuration / usage error (unreadable manifest, asset dir, or C++ source).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "dockview-toolchain.json"

# Symbols the e05a contract-surface module (src/editor/webui/core/src/info.ts) exports, re-exported
# by the e05c app entry (src/editor/webui/core/src/index.ts). Their presence proves the bundle
# carries the real entry, not an empty or unrelated file.
REQUIRED_MARKERS = ("editorCoreInfo", "isRpcMethod", "isEventTopic", "isRetriable")

# Unambiguous Node-runtime tells (see the module docstring, check 2).
NODE_MARKERS = ('"node:', "'node:", "#!/usr/bin/env node")

# The served document set e05c added beside the bundle.
APP_DOCUMENT = "index.html"
APP_STYLESHEET = "app.css"

# The four cross-language constants (check 5). Each entry is
# (human name, C++ file, C++ constant, TS constant) — the VALUE is read from the C++ side, which is
# the authority: the Shell is what actually registers the scheme and routes the queries.
SCHEME_CONSTANTS = (
    ("scheme", "app_scheme.h", "kAppScheme", "BRIDGE_SCHEME"),
    ("origin", "app_scheme.h", "kAppOrigin", "BRIDGE_ORIGIN"),
    ("ipc endpoint", "app_scheme.h", "kIpcEndpoint", "BRIDGE_ENDPOINT"),
)

# The query-function name lives in the CEF binding (it is a CEF message-router config value), not in
# a header — so it is read from there.
QUERY_FUNCTION_CONSTANT = ("query function", "cef_shell.cpp", "kBridgeQueryFunction",
                           "BRIDGE_QUERY_FUNCTION")

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


def _read_cpp_string_constant(source: Path, name: str) -> str:
    """Read a `... name = "value";` C++ string constant. Raises CheckError when absent.

    Deliberately a regex over the source rather than anything cleverer: the alternative is
    generating a shared header, which for four short strings costs more machinery than the drift it
    prevents. The regex is anchored on the constant NAME, so it cannot silently match a neighbour,
    and a rename that breaks it fails LOUDLY (exit 2) instead of quietly matching nothing.
    """
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read {source}: {exc}") from exc
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\"([^\"]*)\"", text)
    if match is None:
        raise CheckError(
            f"constant '{name}' not found in {source} — it was renamed or removed, so the "
            f"cross-language scheme check can no longer verify anything. Update SCHEME_CONSTANTS.")
    return match.group(1)


def _read_ts_constant_from_bundle(bundle_text: str, name: str) -> str | None:
    """Read a `name = "value"` pair out of the BUNDLED JS (post-esbuild, so quotes may be either)."""
    match = re.search(rf"\b{re.escape(name)}\s*=\s*[\"']([^\"']*)[\"']", bundle_text)
    return match.group(1) if match else None


def _strip_html_comments(text: str) -> str:
    """Drop HTML comments before scanning markup.

    LOAD-BEARING, not a nicety: the served documents carry a comment block that DOCUMENTS these
    very rules (it names `<script>`, `style="..."` and `file://` in prose), and scanning it as if it
    were markup produces a false positive on every one of them — which is exactly what happened the
    first time this gate ran. A comment cannot execute, cannot be a fallback, and cannot be a CSP
    violation, so removing it is also semantically right, not merely convenient.
    """
    return re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)


def check_csp_clean_document(document: Path) -> list[str]:
    """Check 4 — the served document carries nothing the CSP would block."""
    failures: list[str] = []
    if not document.is_file():
        return [f"served document missing: {document}"]
    text = _strip_html_comments(document.read_text(encoding="utf-8", errors="replace"))

    # An inline <script> is one with a BODY. `<script type="module" src="...">` is exactly what the
    # document is supposed to have, so the check is on content between the tags, not on the tag.
    for body in re.findall(r"<script\b[^>]*>(.*?)</script\s*>", text, re.IGNORECASE | re.DOTALL):
        if body.strip():
            failures.append(
                f"{document.name} contains an INLINE <script> body — the response carries "
                f"script-src 'self' with no unsafe-inline, so the browser will BLOCK it and the "
                f"editor will load blank (design 04 section 1 / 08 section 2)")
    if re.search(r"<style\b", text, re.IGNORECASE):
        failures.append(
            f"{document.name} contains an inline <style> element — style-src 'self' blocks it; put "
            f"the rules in {APP_STYLESHEET} instead")
    if re.search(r"<[^>]*\sstyle\s*=", text, re.IGNORECASE):
        failures.append(
            f"{document.name} contains an inline style= attribute — style-src 'self' blocks it")
    if "javascript:" in text.lower():
        failures.append(f"{document.name} contains a javascript: URL — blocked by the CSP")
    return failures


def check_scheme_contract(asset_dir: Path, bundle_name: str, shell_include_dir: Path,
                          shell_cef_dir: Path) -> list[str]:
    """Checks 5 + 6 — cross-language scheme vocabulary, and no file:// anywhere in the asset set."""
    failures: list[str] = []

    bundle = asset_dir / bundle_name
    if not bundle.is_file():
        return [f"bundle missing: {bundle}"]
    bundle_text = bundle.read_text(encoding="utf-8", errors="replace")

    checks = list(SCHEME_CONSTANTS)
    for human, cpp_file, cpp_name, ts_name in checks:
        cpp_value = _read_cpp_string_constant(shell_include_dir / cpp_file, cpp_name)
        ts_value = _read_ts_constant_from_bundle(bundle_text, ts_name)
        if ts_value is None:
            failures.append(
                f"{human}: the bundle does not declare {ts_name} — editor-core cannot be using the "
                f"same vocabulary the Shell serves")
        elif ts_value != cpp_value:
            failures.append(
                f"{human} DRIFTED: C++ {cpp_name}={cpp_value!r} but TS {ts_name}={ts_value!r}. The "
                f"Shell and editor-core would disagree about the channel and the bridge would be "
                f"silently unreachable.")

    human, cef_file, cpp_name, ts_name = QUERY_FUNCTION_CONSTANT
    cpp_value = _read_cpp_string_constant(shell_cef_dir / cef_file, cpp_name)
    ts_value = _read_ts_constant_from_bundle(bundle_text, ts_name)
    if ts_value is None:
        failures.append(f"{human}: the bundle does not declare {ts_name}")
    elif ts_value != cpp_value:
        failures.append(
            f"{human} DRIFTED: C++ {cpp_name}={cpp_value!r} but TS {ts_name}={ts_value!r} — the "
            f"Shell would inject one function name and editor-core would look for another, so "
            f"every bridge call would fail with 'not a function'.")

    # Check 6 — the DoD's "no file:// fallback exists", asserted over every served text asset rather
    # than only the document (a fallback smuggled into the bundle is the same defect).
    for asset in sorted(asset_dir.glob("*")):
        if not asset.is_file() or asset.suffix.lower() not in {".html", ".js", ".css", ".mjs"}:
            continue
        text = asset.read_text(encoding="utf-8", errors="replace")
        if asset.suffix.lower() == ".html":
            # Same reason as check 4: the document's own comment block explains the no-file:// rule
            # in prose, and a comment is not a fallback.
            text = _strip_html_comments(text)
        if "file://" in text:
            failures.append(
                f"{asset.name} references file:// — assets ship in-app and load over "
                f"context-editor://, never from a file URL (design 04 section 1)")
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


def run_scheme_contract(asset_dir: Path, bundle_name: str, shell_include_dir: Path,
                        shell_cef_dir: Path) -> int:
    """The M9 e05c gate (checks 4-6). Separate entry point so a failure names the right gate."""
    if not asset_dir.is_dir():
        raise CheckError(f"asset dir does not exist: {asset_dir}")
    if not shell_include_dir.is_dir():
        raise CheckError(f"shell include dir does not exist: {shell_include_dir}")
    if not shell_cef_dir.is_dir():
        raise CheckError(f"shell cef source dir does not exist: {shell_cef_dir}")

    failures = check_csp_clean_document(asset_dir / APP_DOCUMENT)
    stylesheet = asset_dir / APP_STYLESHEET
    if not stylesheet.is_file():
        failures.append(f"served stylesheet missing: {stylesheet}")
    failures.extend(check_scheme_contract(asset_dir, bundle_name, shell_include_dir, shell_cef_dir))

    if failures:
        for failure in failures:
            print(f"[check_webui_assets] FAIL: {failure}", file=sys.stderr)
        return 1

    print(f"[check_webui_assets] OK: {APP_DOCUMENT} is CSP-clean, the scheme vocabulary matches the "
          f"Shell's C++ constants, and no file:// URL is present in {asset_dir}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Verify the built editor-core asset set.")
    parser.add_argument("--asset-dir", type=Path, required=True,
                        help="the built asset dir (bundle + staged dockview files)")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="dockview pin manifest (default: tools/dockview-toolchain.json)")
    parser.add_argument("--bundle-name", default="editor-core.js",
                        help="bundle file name inside --asset-dir")
    parser.add_argument("--scheme-contract", action="store_true",
                        help="run the M9 e05c gate (CSP-clean document + cross-language scheme "
                             "vocabulary + no file:// fallback) instead of the asset checks")
    parser.add_argument("--shell-include-dir", type=Path,
                        default=REPO_ROOT / "src" / "editor" / "shell" / "include" / "context" /
                        "editor" / "shell",
                        help="dir holding app_scheme.h (--scheme-contract only)")
    parser.add_argument("--shell-cef-dir", type=Path,
                        default=REPO_ROOT / "src" / "editor" / "shell" / "cef" / "src",
                        help="dir holding cef_shell.cpp (--scheme-contract only)")
    args = parser.parse_args(argv)

    try:
        if args.scheme_contract:
            return run_scheme_contract(args.asset_dir, args.bundle_name, args.shell_include_dir,
                                       args.shell_cef_dir)
        return run(args.asset_dir, args.manifest, args.bundle_name)
    except CheckError as exc:
        print(f"[check_webui_assets] ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
