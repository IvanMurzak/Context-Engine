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
     inline ``<style>``/``style=``. The Shell serves ``script-src 'self'`` with no ``unsafe-inline``
     — an inline ``<script>`` or event handler is BLOCKED by the browser at runtime, so the failure
     mode of forgetting THAT rule is a blank editor with a console message; this turns it into a
     build error. ``style-src`` DOES carry ``'unsafe-inline'`` (``app_csp_header()`` — the vendored
     dockview-core engine injects a ``<style>`` element and writes CSSOM inline styles at runtime,
     which the pinned CEF/Chromium build enforces through ``style-src``), but the AUTHORED document
     is still kept free of inline styles here as defense-in-depth, so the relaxation's blast radius
     stays the vendored engine and never authored markup.
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

# The message-router function names live in the CEF binding (they are CEF message-router config
# values), not in a header — so they are read from there. BOTH names must be covered: CEF requires
# the browser-side and renderer-side configs to agree, and a rename of either one desyncs the channel
# exactly as a scheme rename would.
CEF_CONSTANTS = (
    ("query function", "cef_shell.cpp", "kBridgeQueryFunction", "BRIDGE_QUERY_FUNCTION"),
    ("cancel function", "cef_shell.cpp", "kBridgeCancelFunction", "BRIDGE_CANCEL_FUNCTION"),
)

# --- the M9 e05d1 PANEL contract (check 7) --------------------------------------------------------
# The `panel.*` method names and the D6 state member names, in the two languages that must agree
# about them. Same authority direction as SCHEME_CONSTANTS: the C++ side is what actually ROUTES, so
# its value is read and the bundle is compared against it.
#
# WHY THIS GATE EXISTS. A rename on either side unbinds the panel surface SILENTLY — the renderer
# calls a method the Shell no longer routes, `panel.list` refuses with `unknown_method`, PanelHost
# reports "no readable roster", and the editor comes up with no panels and NO build error anywhere.
# That is precisely the class of failure e05c's nosniff break taught: a deterministic break with no
# local signal. Here it is mechanised instead.
PANEL_CONSTANTS = (
    ("panel.list", "panel_host.h", "kPanelListMethod", "PANEL_LIST_METHOD"),
    ("panel.render", "panel_host.h", "kPanelRenderMethod", "PANEL_RENDER_METHOD"),
    ("panel.command", "panel_host.h", "kPanelCommandMethod", "PANEL_COMMAND_METHOD"),
    ("panel.gesture", "panel_host.h", "kPanelGestureMethod", "PANEL_GESTURE_METHOD"),
    ("panel.state.get", "panel_host.h", "kPanelStateGetMethod", "PANEL_STATE_GET_METHOD"),
    ("panel.state.set", "panel_host.h", "kPanelStateSetMethod", "PANEL_STATE_SET_METHOD"),
)

# The D6 persisted-blob member names, which live in the GUI contract library rather than the Shell.
PANEL_STATE_CONSTANTS = (
    ("state schemaVersion key", "kStateSchemaVersionKey", "STATE_SCHEMA_VERSION_KEY"),
    ("state data key", "kStateDataKey", "STATE_DATA_KEY"),
)

# The M9 e05d2 editor-state + region-map surface (design 03 §1 / §6), whose vocabulary lives in
# editor_state_bridge.h. SAME silent-drift hazard as the panel surface above: a method renamed on one
# side unbinds persistence with no build error — layout stops saving and NOTHING reports it — and a
# region-kind token renamed on one side mis-routes input. Cross-checked identically, from the header
# constants and the built bundle.
EDITOR_STATE_CONSTANTS = (
    ("editor.state.get", "editor_state_bridge.h", "kEditorStateGetMethod",
     "EDITOR_STATE_GET_METHOD"),
    ("editor.state.publish", "editor_state_bridge.h", "kEditorStatePublishMethod",
     "EDITOR_STATE_PUBLISH_METHOD"),
    ("editor.regions.publish", "editor_state_bridge.h", "kEditorRegionsPublishMethod",
     "EDITOR_REGIONS_PUBLISH_METHOD"),
    ("region kind viewport", "editor_state_bridge.h", "kRegionKindViewport", "REGION_KIND_VIEWPORT"),
    ("region kind native", "editor_state_bridge.h", "kRegionKindNative", "REGION_KIND_NATIVE"),
)

# The closed gesture vocabulary (04 §4), compared SET vs SET between the C++ wire tokens and the
# bundle's own GESTURE_VERBS array, so a verb added on one side without the other — which would be
# refused at runtime with no build error — fails here instead.
#
# READ FROM `panel_host.cpp`, NOT THE HEADER. The header holds only the ENUMERATOR names (`begin,`
# `extend,` ...) and prose; a substring probe against it is satisfied unconditionally and can never
# fail, so it would report OK on a genuine drift. The tokens that actually go on the wire are the
# `return "<tok>";` literals in `gesture_verb_token`'s switch — those are the authority.
#
# This tuple is this script's own PIN of the vocabulary: when the C++ set stops matching it, that is
# an anti-rot CheckError (exit 2), not a drift failure — the check can no longer judge either side.
GESTURE_VERBS = ("begin", "extend", "commit", "cancel")

# The pinned docking engine must be LOADED BY THE DOCUMENT, not bundled. Asserting the script tag is
# what keeps `webui-assets`' SHA re-hash of the staged file meaningful: if the engine were ever
# folded into editor-core.js, the verified artifact would stop being the shipped one while the pin
# check kept passing over a file nothing loads.
DOCKVIEW_SCRIPT = "dockview-core.min.js"

# The editor-core npm dependency allowlist (the s1-approved set, 04 §2 / 08 §3). Owner-ratified and
# VERSION-PINNED: a bump past 7.0.2, or any additional dockview-* package, re-triggers the standing
# consent gate. Checked from `package.json` so a dependency added there — the one place the license
# gate also reads — cannot slip in without tripping this.
EDITOR_CORE_DEPENDENCIES = {"dockview-core": "7.0.2"}

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


def _read_cpp_gesture_verbs(source: Path) -> tuple[str, ...]:
    """Read the WIRE TOKENS out of the C++ `gesture_verb_token` switch body.

    Deliberately reads the `.cpp`, not the header. The header carries only the ENUMERATOR names
    (`begin,` `extend,` ...) and prose; a bare substring check against it can never fail, because
    the enumerators guarantee every verb's substring unconditionally. The tokens that actually go
    on the wire are the `return "<tok>";` literals in this switch, so those are what must be
    compared against the bundle. Anchored on the function name so a neighbouring function's
    returns cannot leak in, and a rename fails LOUDLY (exit 2) rather than matching nothing.
    """
    try:
        text = source.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read {source}: {exc}") from exc
    body = re.search(r"gesture_verb_token\s*\([^)]*\)\s*\{(.*?)\n\}", text, re.DOTALL)
    if body is None:
        raise CheckError(
            f"`gesture_verb_token` not found in {source} — the closed gesture vocabulary moved, so "
            f"this check can no longer verify anything. Update the check to its new home.")
    verbs = tuple(dict.fromkeys(re.findall(r"return\s+\"([^\"]*)\"", body.group(1))))
    if not verbs:
        raise CheckError(
            f"`gesture_verb_token` in {source} returns no string literals — the vocabulary is no "
            f"longer expressed as returned tokens, so this check would pass vacuously.")
    return verbs


def _read_ts_string_array_from_bundle(bundle_text: str, name: str) -> tuple[str, ...] | None:
    """Read a `name = ["a", "b"]` TS string array out of the BUILT bundle.

    Anchored on the array so the comparison is set-vs-set. A bare `'"commit"' in bundle_text`
    substring probe would be satisfied by the verb appearing anywhere at all — including in an
    unrelated string or a comment the minifier kept — which is not evidence that the runtime's
    gesture vocabulary contains it.
    """
    match = re.search(rf"\b{re.escape(name)}\s*=\s*\[([^\]]*)\]", bundle_text)
    if match is None:
        return None
    # Either quoting style survives bundling, so accept both and keep declaration order.
    return tuple(double or single
                 for double, single in re.findall(r"\"([^\"]*)\"|'([^']*)'", match.group(1)))


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
            f"{document.name} contains an inline <style> element — editor-core's authored documents "
            f"are kept inline-style-free as defense-in-depth (the response's style-src 'unsafe-inline' "
            f"exists only for the vendored dockview-core engine's runtime DOM, not authored markup); "
            f"put the rules in {APP_STYLESHEET} instead")
    if re.search(r"<[^>]*\sstyle\s*=", text, re.IGNORECASE):
        failures.append(
            f"{document.name} contains an inline style= attribute — the authored document is kept "
            f"free of inline styles as defense-in-depth; the response's style-src 'unsafe-inline' is "
            f"relaxed only for the vendored dockview-core engine, not for authored markup")
    # An inline event handler is inline SCRIPT by another spelling: script-src 'self' with no
    # unsafe-inline blocks `onclick="..."` exactly as it blocks an inline <script> body. Checking
    # `javascript:` but not `on*=` left the more common of the two uncovered.
    if re.search(r"<[^>]*\son[a-z]+\s*=", text, re.IGNORECASE):
        failures.append(
            f"{document.name} contains an inline event handler (on...=) — script-src 'self' with "
            f"no unsafe-inline blocks it; bind the listener from the bundle instead")
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

    for human, cpp_file, cpp_name, ts_name in SCHEME_CONSTANTS:
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

    for human, cef_file, cpp_name, ts_name in CEF_CONSTANTS:
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


def check_panel_contract(asset_dir: Path, bundle_name: str, shell_include_dir: Path,
                         contract_include_dir: Path, package_json: Path,
                         shell_src_dir: Path) -> list[str]:
    """Check 7 — the M9 e05d1 panel surface: cross-language vocabulary, engine loading, deps."""
    failures: list[str] = []

    bundle = asset_dir / bundle_name
    if not bundle.is_file():
        return [f"bundle missing: {bundle}"]
    bundle_text = bundle.read_text(encoding="utf-8", errors="replace")

    # 7a — the `panel.*` method names agree across the two languages.
    for human, cpp_file, cpp_name, ts_name in PANEL_CONSTANTS:
        cpp_value = _read_cpp_string_constant(shell_include_dir / cpp_file, cpp_name)
        ts_value = _read_ts_constant_from_bundle(bundle_text, ts_name)
        if ts_value is None:
            failures.append(
                f"{human}: the bundle does not declare {ts_name} — the hydration runtime cannot be "
                f"calling the method the Shell routes")
        elif ts_value != cpp_value:
            failures.append(
                f"{human} DRIFTED: C++ {cpp_name}={cpp_value!r} but TS {ts_name}={ts_value!r}. The "
                f"Shell would route one name and editor-core would call another, so the panel would "
                f"never mount and NOTHING would report an error.")

    # 7a2 — the e05d2 editor-state + region-map vocabulary agrees across the two languages. Same
    # mechanism as 7a; a rename here unbinds persistence (layout stops saving) or mis-routes input.
    for human, cpp_file, cpp_name, ts_name in EDITOR_STATE_CONSTANTS:
        cpp_value = _read_cpp_string_constant(shell_include_dir / cpp_file, cpp_name)
        ts_value = _read_ts_constant_from_bundle(bundle_text, ts_name)
        if ts_value is None:
            failures.append(
                f"{human}: the bundle does not declare {ts_name} — editor-core cannot be calling the "
                f"method / emitting the token the Shell routes")
        elif ts_value != cpp_value:
            failures.append(
                f"{human} DRIFTED: C++ {cpp_name}={cpp_value!r} but TS {ts_name}={ts_value!r}. The "
                f"Shell would route/parse one value and editor-core would send another, so layout "
                f"persistence (or region-map input routing) would break with NO error anywhere.")

    # 7b — the D6 persisted-blob member names agree (gui/contract/panel_state.h is their authority).
    for human, cpp_name, ts_name in PANEL_STATE_CONSTANTS:
        cpp_value = _read_cpp_string_constant(contract_include_dir / "panel_state.h", cpp_name)
        ts_value = _read_ts_constant_from_bundle(bundle_text, ts_name)
        if ts_value is None:
            failures.append(f"{human}: the bundle does not declare {ts_name}")
        elif ts_value != cpp_value:
            failures.append(
                f"{human} DRIFTED: C++ {cpp_name}={cpp_value!r} but TS {ts_name}={ts_value!r} — a "
                f"persisted panel state would be written under one key and read under another, so "
                f"every restore would degrade to the D6 null-state path.")

    # 7c — the closed gesture vocabulary, compared SET vs SET between the C++ wire tokens and the
    # bundle's own array. Both sides are read from where the values actually live: the tokens from
    # `gesture_verb_token`'s returns in panel_host.cpp, the array from the BUILT bundle.
    cpp_verbs = _read_cpp_gesture_verbs(shell_src_dir / "panel_host.cpp")
    if set(cpp_verbs) != set(GESTURE_VERBS):
        # An anti-rot assertion, not a drift failure: this script's own pinned vocabulary is stale,
        # so it can no longer be trusted to judge either side. Loud (exit 2), never a quiet pass.
        raise CheckError(
            f"the C++ gesture vocabulary is {sorted(cpp_verbs)} but this check pins "
            f"{sorted(GESTURE_VERBS)} — the closed vocabulary changed, so update GESTURE_VERBS "
            f"(and the hydration runtime) deliberately.")
    ts_verbs = _read_ts_string_array_from_bundle(bundle_text, "GESTURE_VERBS")
    if ts_verbs is None:
        failures.append(
            "the bundle does not declare a GESTURE_VERBS array — the hydration runtime cannot emit "
            "a verb vocabulary it does not name, so that half of the gesture contract is dead")
    elif set(ts_verbs) != set(cpp_verbs):
        missing = sorted(set(cpp_verbs) - set(ts_verbs))
        extra = sorted(set(ts_verbs) - set(cpp_verbs))
        failures.append(
            f"gesture vocabulary DRIFTED: C++ `gesture_verb_token` emits {sorted(cpp_verbs)} but "
            f"the bundle's GESTURE_VERBS is {sorted(ts_verbs)}"
            + (f" (missing from TS: {missing})" if missing else "")
            + (f" (unknown to C++: {extra})" if extra else "")
            + " — a verb one side sends and the other does not accept is refused at runtime with "
              "no build error, which is exactly what this gate exists to catch.")

    # 7d — the pinned engine is LOADED by the document rather than bundled (see DOCKVIEW_SCRIPT).
    document = asset_dir / APP_DOCUMENT
    if not document.is_file():
        failures.append(f"served document missing: {document}")
    else:
        markup = _strip_html_comments(document.read_text(encoding="utf-8", errors="replace"))
        if not re.search(rf"<script\b[^>]*\bsrc\s*=\s*[\"'][^\"']*{re.escape(DOCKVIEW_SCRIPT)}",
                         markup, re.IGNORECASE):
            failures.append(
                f"{APP_DOCUMENT} does not load {DOCKVIEW_SCRIPT} — PanelHost reads the docking "
                f"engine off the UMD global that script publishes, so no panel can mount")
        if not (asset_dir / DOCKVIEW_SCRIPT).is_file():
            failures.append(f"the pinned docking engine is not staged: {asset_dir / DOCKVIEW_SCRIPT}")

    # 7e — the editor-core dependency allowlist (the s1-approved set, owner-ratified + version-pinned).
    try:
        package = json.loads(package_json.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CheckError(f"cannot read {package_json}: {exc}") from exc
    declared = package.get("dependencies", {})
    if not isinstance(declared, dict):
        raise CheckError(f"{package_json}: 'dependencies' is not an object")
    if declared != EDITOR_CORE_DEPENDENCIES:
        failures.append(
            f"editor-core dependencies DRIFTED from the s1-approved set: expected "
            f"{EDITOR_CORE_DEPENDENCIES}, found {declared}. A bump past the pinned version, or any "
            f"additional package, re-triggers the standing owner-consent gate (design 08 section 3) "
            f"— it is not a code change to make on the way past.")
    return failures


def run_panel_contract(asset_dir: Path, bundle_name: str, shell_include_dir: Path,
                       contract_include_dir: Path, package_json: Path,
                       shell_src_dir: Path) -> int:
    """The M9 e05d1 gate (check 7). Separate entry point so a failure names the right gate."""
    if not asset_dir.is_dir():
        raise CheckError(f"asset dir does not exist: {asset_dir}")
    if not shell_include_dir.is_dir():
        raise CheckError(f"shell include dir does not exist: {shell_include_dir}")
    if not contract_include_dir.is_dir():
        raise CheckError(f"gui contract include dir does not exist: {contract_include_dir}")

    if not shell_src_dir.is_dir():
        raise CheckError(f"shell src dir does not exist: {shell_src_dir}")

    failures = check_panel_contract(asset_dir, bundle_name, shell_include_dir, contract_include_dir,
                                    package_json, shell_src_dir)
    if failures:
        for failure in failures:
            print(f"[check_webui_assets] FAIL: {failure}", file=sys.stderr)
        return 1

    print(f"[check_webui_assets] OK: the panel.* vocabulary, the D6 state keys and the gesture verbs "
          f"match the Shell's C++ constants; {DOCKVIEW_SCRIPT} is loaded by the document and the "
          f"editor-core dependency set is the s1-approved one")
    return 0


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
    parser.add_argument("--panel-contract", action="store_true",
                        help="run the M9 e05d1 gate (cross-language panel.* vocabulary + D6 state "
                             "keys + gesture verbs + docking-engine loading + the editor-core "
                             "dependency allowlist) instead of the asset checks")
    parser.add_argument("--contract-include-dir", type=Path,
                        default=REPO_ROOT / "src" / "editor" / "gui" / "contract" / "include" /
                        "context" / "editor" / "gui" / "contract",
                        help="dir holding panel_state.h (--panel-contract only)")
    parser.add_argument("--package-json", type=Path,
                        default=REPO_ROOT / "src" / "editor" / "webui" / "core" / "package.json",
                        help="editor-core's package.json (--panel-contract only)")
    parser.add_argument("--shell-src-dir", type=Path,
                        default=REPO_ROOT / "src" / "editor" / "shell" / "src",
                        help="dir holding panel_host.cpp, whose `gesture_verb_token` switch is the "
                             "authority on the wire verbs (--panel-contract only)")
    args = parser.parse_args(argv)

    try:
        if args.scheme_contract:
            return run_scheme_contract(args.asset_dir, args.bundle_name, args.shell_include_dir,
                                       args.shell_cef_dir)
        if args.panel_contract:
            return run_panel_contract(args.asset_dir, args.bundle_name, args.shell_include_dir,
                                      args.contract_include_dir, args.package_json,
                                      args.shell_src_dir)
        return run(args.asset_dir, args.manifest, args.bundle_name)
    except CheckError as exc:
        print(f"[check_webui_assets] ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
