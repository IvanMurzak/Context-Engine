"""Tests for tools/check_webui_assets.py — the built editor-core asset-set gate (R-QA-013, M9 e05a).

Covers the happy path and every failure this gate exists to catch: a missing/empty/incomplete
bundle, a Node reference leaking into a browser asset, and — the supply-chain half — a staged
dockview file that no longer matches its pin (verify-AT-USE, one step beyond fetch_dockview.py's
verify-before-use).

A dedicated block pins the "narrow Node markers" decision: `require(`/`module.exports` must NOT
fail the gate, because e05c bundles dockview's UMD build whose CommonJS shim contains exactly those
tokens while depending on Node not at all. Only `node:` specifiers and a node shebang are treated
as Node tells. Without these tests that distinction would be re-litigated (or silently broken) the
first time someone tightened the check.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

import pytest
from conftest import load_tool

check_webui_assets = load_tool("check_webui_assets")

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REAL_MANIFEST = REPO_ROOT / "tools" / "dockview-toolchain.json"

GOOD_BUNDLE = """\
// bundled by esbuild
function editorCoreInfo() { return {}; }
function isRpcMethod(v) { return true; }
function isEventTopic(v) { return true; }
function isRetriable(v) { return false; }
export { editorCoreInfo, isRpcMethod, isEventTopic, isRetriable };
"""

DOCKVIEW_JS = b"/* dockview-core */\n"
DOCKVIEW_CSS = b".dv-dockview{}\n"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _manifest(tmp_path: Path, *, js: bytes = DOCKVIEW_JS, css: bytes = DOCKVIEW_CSS) -> Path:
    manifest = {
        "package": "dockview-core",
        "version": "7.0.2",
        "members": {
            "dockview-core.min.js": {"member": "package/dist/dockview-core.min.js",
                                     "sha256": _sha(js)},
            "dockview.css": {"member": "package/dist/styles/dockview.css", "sha256": _sha(css)},
        },
    }
    path = tmp_path / "dockview-toolchain.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def _assets(tmp_path: Path, *, bundle: str | None = GOOD_BUNDLE,
            js: bytes = DOCKVIEW_JS, css: bytes = DOCKVIEW_CSS) -> Path:
    asset_dir = tmp_path / "app"
    asset_dir.mkdir(parents=True, exist_ok=True)
    if bundle is not None:
        (asset_dir / "editor-core.js").write_text(bundle, encoding="utf-8")
    (asset_dir / "dockview-core.min.js").write_bytes(js)
    (asset_dir / "dockview.css").write_bytes(css)
    return asset_dir


# --- happy path ----------------------------------------------------------------------------------

def test_complete_asset_set_passes(tmp_path: Path, capsys) -> None:
    assert check_webui_assets.run(_assets(tmp_path), _manifest(tmp_path), "editor-core.js") == 0
    assert "OK" in capsys.readouterr().out


# --- bundle assertions ---------------------------------------------------------------------------

def test_missing_bundle_fails(tmp_path: Path) -> None:
    asset_dir = _assets(tmp_path, bundle=None)
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


def test_empty_bundle_fails(tmp_path: Path) -> None:
    """esbuild exiting 0 does not prove it wrote anything meaningful."""
    asset_dir = _assets(tmp_path, bundle="   \n\n  ")
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


@pytest.mark.parametrize("missing", ["editorCoreInfo", "isRpcMethod", "isEventTopic", "isRetriable"])
def test_bundle_missing_an_entry_export_fails(tmp_path: Path, missing: str) -> None:
    asset_dir = _assets(tmp_path, bundle=GOOD_BUNDLE.replace(missing, "somethingElse"))
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


@pytest.mark.parametrize("node_ref", [
    'import fs from "node:fs";',
    "import fs from 'node:fs';",
    "#!/usr/bin/env node",
])
def test_node_reference_in_the_bundle_fails(tmp_path: Path, node_ref: str) -> None:
    asset_dir = _assets(tmp_path, bundle=node_ref + "\n" + GOOD_BUNDLE)
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


@pytest.mark.parametrize("commonjs", [
    'var x = require("./thing");',
    "module.exports = {};",
    "typeof exports === 'object'",
])
def test_commonjs_shims_do_not_fail_the_node_check(tmp_path: Path, commonjs: str) -> None:
    """Deliberate scope limit: dockview's UMD build (bundled from e05c) carries a CommonJS shim and
    depends on Node not at all. Tightening the gate to `require(` would red a correct build."""
    asset_dir = _assets(tmp_path, bundle=commonjs + "\n" + GOOD_BUNDLE)
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 0


# --- dockview supply-chain assertions ------------------------------------------------------------

def test_missing_dockview_asset_fails(tmp_path: Path) -> None:
    asset_dir = _assets(tmp_path)
    (asset_dir / "dockview.css").unlink()
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


def test_altered_dockview_asset_fails(tmp_path: Path) -> None:
    """verify-AT-USE: a file swapped AFTER fetch_dockview.py verified it is still caught."""
    manifest = _manifest(tmp_path)
    asset_dir = _assets(tmp_path)
    (asset_dir / "dockview-core.min.js").write_bytes(b"/* tampered */\n")
    assert check_webui_assets.run(asset_dir, manifest, "editor-core.js") == 1


def test_altered_dockview_css_fails(tmp_path: Path) -> None:
    manifest = _manifest(tmp_path)
    asset_dir = _assets(tmp_path)
    (asset_dir / "dockview.css").write_bytes(b".dv-dockview{content:'evil'}\n")
    assert check_webui_assets.run(asset_dir, manifest, "editor-core.js") == 1


# --- config errors -------------------------------------------------------------------------------

def test_missing_asset_dir_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(check_webui_assets.CheckError, match="asset dir"):
        check_webui_assets.run(tmp_path / "absent", _manifest(tmp_path), "editor-core.js")


def test_unreadable_manifest_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(check_webui_assets.CheckError):
        check_webui_assets.run(_assets(tmp_path), tmp_path / "absent.json", "editor-core.js")


def test_manifest_without_members_is_a_config_error(tmp_path: Path) -> None:
    path = tmp_path / "m.json"
    path.write_text(json.dumps({"package": "dockview-core"}), encoding="utf-8")
    with pytest.raises(check_webui_assets.CheckError, match="members"):
        check_webui_assets.run(_assets(tmp_path), path, "editor-core.js")


def test_member_without_a_sha_pin_is_a_config_error(tmp_path: Path) -> None:
    path = tmp_path / "m.json"
    path.write_text(json.dumps(
        {"members": {"dockview.css": {"member": "package/dist/styles/dockview.css"}}}),
        encoding="utf-8")
    with pytest.raises(check_webui_assets.CheckError, match="sha256"):
        check_webui_assets.run(_assets(tmp_path), path, "editor-core.js")


# --- CLI exit-code surface -----------------------------------------------------------------------

def test_main_returns_0_on_success(tmp_path: Path) -> None:
    assert check_webui_assets.main([
        "--asset-dir", str(_assets(tmp_path)),
        "--manifest", str(_manifest(tmp_path))]) == 0


def test_main_returns_1_on_failure(tmp_path: Path, capsys) -> None:
    asset_dir = _assets(tmp_path, bundle=None)
    assert check_webui_assets.main([
        "--asset-dir", str(asset_dir), "--manifest", str(_manifest(tmp_path))]) == 1
    assert "FAIL" in capsys.readouterr().err


def test_main_returns_2_on_config_error(tmp_path: Path, capsys) -> None:
    assert check_webui_assets.main([
        "--asset-dir", str(tmp_path / "absent"), "--manifest", str(_manifest(tmp_path))]) == 2
    assert "ERROR" in capsys.readouterr().err


# --- integration with the REAL manifest ----------------------------------------------------------

def test_real_manifest_members_are_checkable() -> None:
    """The gate's member list must stay in step with the real pin manifest."""
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    assert set(manifest["members"]) == {"dockview-core.min.js", "dockview.css"}
    for spec in manifest["members"].values():
        assert len(spec["sha256"]) == 64


# --- the M9 e05c scheme-contract gate (--scheme-contract) ----------------------------------------
#
# Three properties that live in two languages (the C++ Shell + the TS bundle) plus one hand-authored
# HTML document, none of which any compiler cross-checks. The gate's own FAILURE mode matters as
# much as its success here: the served document carries a comment block that describes these rules
# in prose (it names `<script>`, a style attribute and `file://`), and the first implementation
# reported all three from inside that comment. That regression is pinned below.

GOOD_DOCUMENT = (
    "<!DOCTYPE html>\n"
    '<html lang="en">\n'
    '<head><meta charset="utf-8"><link rel="stylesheet" href="./app.css"></head>\n'
    '<body><main id="editor-root"></main>\n'
    '<script type="module" src="./editor-core.js"></script>\n'
    "</body>\n"
    "</html>\n"
)

GOOD_STYLESHEET = ":root { --editor-bg: #132a44; }\n"

# The five constants the bundle must agree with the Shell about.
SCHEME_BUNDLE = (
    'var BRIDGE_SCHEME = "context-editor";\n'
    'var BRIDGE_ORIGIN = "context-editor://app";\n'
    'var BRIDGE_ENDPOINT = "context-editor://ipc";\n'
    'var BRIDGE_QUERY_FUNCTION = "contextEditorQuery";\n'
    'var BRIDGE_CANCEL_FUNCTION = "contextEditorQueryCancel";\n'
)

CPP_HEADER = (
    'inline constexpr const char* kAppScheme = "context-editor";\n'
    'inline constexpr const char* kAppOrigin = "context-editor://app";\n'
    'inline constexpr const char* kIpcEndpoint = "context-editor://ipc";\n'
)

CPP_CEF = (
    'constexpr const char* kBridgeQueryFunction = "contextEditorQuery";\n'
    'constexpr const char* kBridgeCancelFunction = "contextEditorQueryCancel";\n'
)


def _scheme_fixture(tmp_path: Path, *, document: str = GOOD_DOCUMENT,
                    stylesheet: str | None = GOOD_STYLESHEET, bundle: str = SCHEME_BUNDLE,
                    header: str = CPP_HEADER, cef: str = CPP_CEF) -> tuple[Path, Path, Path]:
    asset_dir = tmp_path / "app"
    asset_dir.mkdir(parents=True, exist_ok=True)
    (asset_dir / "editor-core.js").write_text(bundle, encoding="utf-8")
    (asset_dir / "index.html").write_text(document, encoding="utf-8")
    if stylesheet is not None:
        (asset_dir / "app.css").write_text(stylesheet, encoding="utf-8")

    include_dir = tmp_path / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "app_scheme.h").write_text(header, encoding="utf-8")

    cef_dir = tmp_path / "cefsrc"
    cef_dir.mkdir(parents=True, exist_ok=True)
    (cef_dir / "cef_shell.cpp").write_text(cef, encoding="utf-8")
    return asset_dir, include_dir, cef_dir


def _run_scheme(tmp_path: Path, **kwargs) -> int:
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path, **kwargs)
    return check_webui_assets.run_scheme_contract(asset_dir, "editor-core.js", include_dir, cef_dir)


def test_scheme_contract_happy_path(tmp_path: Path, capsys) -> None:
    assert _run_scheme(tmp_path) == 0
    assert "OK" in capsys.readouterr().out


def test_documentation_comments_do_not_trip_the_gate(tmp_path: Path) -> None:
    """The served document DOCUMENTS these rules in prose; a comment is not a violation.

    Regression guard. The first implementation matched the `<script>` mentioned inside the comment
    and then ran its lazy `(.*?)</script>` all the way to the REAL closing tag, reporting a huge
    inline body that did not exist — and flagged the comment's `file://` too. Both were false
    positives on a correct document, and both would have blocked a correct build.
    """
    documented = GOOD_DOCUMENT.replace(
        "<head>",
        "<!--\n  NO INLINE SCRIPT: an inline <script> or a style= attribute is BLOCKED by the CSP,\n"
        "  and assets never load from a file:// temp file.\n-->\n<head>",
        1)
    assert _run_scheme(tmp_path, document=documented) == 0


@pytest.mark.parametrize("bad_document", [
    # An inline <script> BODY (the tag WITH a src= and no body is the correct shape).
    GOOD_DOCUMENT.replace('<script type="module" src="./editor-core.js"></script>',
                          "<script>window.x = 1;</script>"),
    # An inline <style> element.
    GOOD_DOCUMENT.replace("<body>", "<style>body{color:red}</style><body>"),
    # An inline style= attribute.
    GOOD_DOCUMENT.replace('<main id="editor-root">', '<main id="editor-root" style="color:red">'),
    # A javascript: URL.
    GOOD_DOCUMENT.replace('<main id="editor-root">',
                          '<a href="javascript:alert(1)">x</a><main id="editor-root">'),
    # An inline event handler — inline script by another spelling, blocked by script-src 'self'
    # with no unsafe-inline exactly as an inline <script> body is.
    GOOD_DOCUMENT.replace('<main id="editor-root">', '<main id="editor-root" onclick="x()">'),
    GOOD_DOCUMENT.replace('<main id="editor-root">',
                          '<img src="./x.png" onerror="steal()"><main id="editor-root">'),
])
def test_csp_violating_document_fails(tmp_path: Path, bad_document: str) -> None:
    """Each of these must fail the gate at BUILD time. Four are BLOCKED by the served CSP at runtime
    (inline <script> body, javascript: URL, two inline event handlers). The inline <style> element
    and the inline `style=` attribute are NOT blocked at runtime — `style-src 'self' 'unsafe-inline'`
    tolerates inline CSS for the vendored dockview-core engine — but the AUTHORED document is kept
    free of both as defense-in-depth, so the gate rejects them either way."""
    assert _run_scheme(tmp_path, document=bad_document) == 1


def test_missing_stylesheet_fails(tmp_path: Path) -> None:
    assert _run_scheme(tmp_path, stylesheet=None) == 1


def test_missing_document_fails(tmp_path: Path) -> None:
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path)
    (asset_dir / "index.html").unlink()
    assert check_webui_assets.run_scheme_contract(
        asset_dir, "editor-core.js", include_dir, cef_dir) == 1


def test_missing_bundle_fails_the_scheme_gate(tmp_path: Path) -> None:
    """The gate reads the BUILT bundle, so a missing one must fail closed rather than pass vacuously.

    Without this, a build that produced no bundle would sail through the cross-language drift half
    with nothing to compare — the exact shape of a gate that reports green while checking nothing.
    """
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path)
    (asset_dir / "editor-core.js").unlink()
    assert check_webui_assets.run_scheme_contract(
        asset_dir, "editor-core.js", include_dir, cef_dir) == 1


@pytest.mark.parametrize("ts_name,drifted", [
    ("BRIDGE_SCHEME", "context-edit"),
    ("BRIDGE_ORIGIN", "context-editor://application"),
    ("BRIDGE_ENDPOINT", "context-editor://bridge"),
    ("BRIDGE_QUERY_FUNCTION", "cefQuery"),
    # CEF requires the browser-side and renderer-side router configs to agree, so a cancel-function
    # rename desyncs the channel exactly as a query-function rename does.
    ("BRIDGE_CANCEL_FUNCTION", "cefQueryCancel"),
])
def test_scheme_vocabulary_drift_fails(tmp_path: Path, ts_name: str, drifted: str) -> None:
    """A rename on either side must RED, not produce a silently unreachable bridge."""
    bundle = re.sub(rf'{ts_name} = "[^"]*"', f'{ts_name} = "{drifted}"', SCHEME_BUNDLE)
    assert _run_scheme(tmp_path, bundle=bundle) == 1


def test_bundle_missing_a_scheme_constant_fails(tmp_path: Path) -> None:
    bundle = "".join(line + "\n" for line in SCHEME_BUNDLE.splitlines()
                     if "BRIDGE_ENDPOINT" not in line)
    assert _run_scheme(tmp_path, bundle=bundle) == 1


@pytest.mark.parametrize("asset,content", [
    ("index.html", GOOD_DOCUMENT.replace("./editor-core.js", "file:///c:/tmp/editor-core.js")),
    ("editor-core.js", SCHEME_BUNDLE + 'var fallback = "file:///c:/tmp/app";\n'),
    ("app.css", GOOD_STYLESHEET + '@import url("file:///c:/tmp/x.css");\n'),
])
def test_file_url_anywhere_in_the_asset_set_fails(tmp_path: Path, asset: str, content: str) -> None:
    """The DoD line "no file:// fallback exists", over EVERY served text asset, not just the doc."""
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path)
    (asset_dir / asset).write_text(content, encoding="utf-8")
    assert check_webui_assets.run_scheme_contract(
        asset_dir, "editor-core.js", include_dir, cef_dir) == 1


def test_renamed_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """A vanished C++ constant means the gate can verify NOTHING — say so loudly (exit 2).

    The dangerous failure mode is the opposite: a regex that silently matches nothing while the
    gate still reports success, which is how a cross-language check rots into a no-op.
    """
    with pytest.raises(check_webui_assets.CheckError):
        _run_scheme(tmp_path, header=CPP_HEADER.replace("kIpcEndpoint", "kIpcEndpointRenamed"))


def test_missing_asset_dir_is_a_scheme_config_error(tmp_path: Path) -> None:
    _, include_dir, cef_dir = _scheme_fixture(tmp_path)
    with pytest.raises(check_webui_assets.CheckError):
        check_webui_assets.run_scheme_contract(
            tmp_path / "nope", "editor-core.js", include_dir, cef_dir)


def test_main_routes_the_scheme_contract_flag(tmp_path: Path) -> None:
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path)
    assert check_webui_assets.main([
        "--asset-dir", str(asset_dir), "--bundle-name", "editor-core.js",
        "--scheme-contract",
        "--shell-include-dir", str(include_dir), "--shell-cef-dir", str(cef_dir),
    ]) == 0


def test_the_real_repo_sources_agree_across_languages() -> None:
    """Cross-check the COMMITTED sources against each other, independent of any build.

    The `webui-scheme-contract` ctest runs this over the BUILT asset dir; this runs it over the
    repo, so a drift introduced without building still reds pytest.
    """
    header = REPO_ROOT / "src" / "editor" / "shell" / "include" / "context" / "editor" / "shell"
    cef = REPO_ROOT / "src" / "editor" / "shell" / "cef" / "src"
    ts = (REPO_ROOT / "src" / "editor" / "webui" / "core" / "src" / "bridge.ts").read_text(
        encoding="utf-8")
    for _human, cpp_file, cpp_name, ts_name in check_webui_assets.SCHEME_CONSTANTS:
        cpp_value = check_webui_assets._read_cpp_string_constant(header / cpp_file, cpp_name)
        assert f'{ts_name} = "{cpp_value}"' in ts, f"{ts_name} drifted from C++ {cpp_name}"
    for _human, cef_file, cpp_name, ts_name in check_webui_assets.CEF_CONSTANTS:
        cpp_value = check_webui_assets._read_cpp_string_constant(cef / cef_file, cpp_name)
        assert f'{ts_name} = "{cpp_value}"' in ts, f"{ts_name} drifted from C++ {cpp_name}"


# --- the M9 e05d1 panel-contract gate (--panel-contract) -----------------------------------------
#
# The panel surface is the WIDEST cross-language seam in the editor: six `panel.*` method names, two
# D6 state member names and four gesture verbs, each existing once in C++ and once in TS, with no
# compiler on either side checking the other. A rename unbinds the whole panel layer SILENTLY — the
# editor comes up with no panels and NOTHING reports an error. These cases pin that the gate catches
# each class of drift, and — just as importantly — that it fails LOUDLY (exit 2) rather than
# degrading into a no-op when the constants it reads are renamed out from under it.

PANEL_BUNDLE = (
    'var PANEL_LIST_METHOD = "panel.list";\n'
    'var PANEL_RENDER_METHOD = "panel.render";\n'
    'var PANEL_COMMAND_METHOD = "panel.command";\n'
    'var PANEL_GESTURE_METHOD = "panel.gesture";\n'
    'var PANEL_STATE_GET_METHOD = "panel.state.get";\n'
    'var PANEL_STATE_SET_METHOD = "panel.state.set";\n'
    'var STATE_SCHEMA_VERSION_KEY = "schemaVersion";\n'
    'var STATE_DATA_KEY = "data";\n'
    'var GESTURE_VERBS = ["begin", "extend", "commit", "cancel"];\n'
    # e05d2 editor-state + region-map vocabulary (editorstate.ts).
    'var EDITOR_STATE_GET_METHOD = "editor.state.get";\n'
    'var EDITOR_STATE_PUBLISH_METHOD = "editor.state.publish";\n'
    'var EDITOR_REGIONS_PUBLISH_METHOD = "editor.regions.publish";\n'
    'var EDITOR_LAYOUT_RESTORED_METHOD = "editor.layout.restored";\n'
    'var REGION_KIND_VIEWPORT = "viewport";\n'
    'var REGION_KIND_NATIVE = "native";\n'
    # e07c keybindings vocabulary (keymap.ts).
    'var KEYBINDINGS_GET_METHOD = "keybindings.get";\n'
    # e06b themes vocabulary (theme.ts).
    'var THEMES_GET_METHOD = "themes.get";\n'
)

# MIRRORS THE REAL HEADER'S SHAPE. The real `panel_host.h` declares the enum and the token
# function; it holds NO wire-token string literals. A fixture that inlined the switch here would be
# MORE CAPABLE THAN THE REAL SOURCE — the defect class that lets a gate pass its own tests while
# being vacuous against the repo (a mutation of a literal the real file does not contain is a no-op).
PANEL_CPP_HEADER = (
    'inline constexpr const char* kPanelListMethod = "panel.list";\n'
    'inline constexpr const char* kPanelRenderMethod = "panel.render";\n'
    'inline constexpr const char* kPanelCommandMethod = "panel.command";\n'
    'inline constexpr const char* kPanelGestureMethod = "panel.gesture";\n'
    'inline constexpr const char* kPanelStateGetMethod = "panel.state.get";\n'
    'inline constexpr const char* kPanelStateSetMethod = "panel.state.set";\n'
    "enum class GestureVerb { begin, extend, commit, cancel };\n"
    "const char* gesture_verb_token(GestureVerb verb);\n"
)

# The wire tokens live in the .cpp switch, which is what the gate must read.
PANEL_CPP_SOURCE = (
    "const char* gesture_verb_token(GestureVerb verb)\n"
    "{\n"
    "    switch (verb)\n"
    "    {\n"
    "    case GestureVerb::begin:\n"
    '        return "begin";\n'
    "    case GestureVerb::extend:\n"
    '        return "extend";\n'
    "    case GestureVerb::commit:\n"
    '        return "commit";\n'
    "    case GestureVerb::cancel:\n"
    '        return "cancel";\n'
    "    }\n"
    '    return "cancel";\n'
    "}\n"
)

PANEL_CPP_STATE = (
    'inline constexpr const char* kStateSchemaVersionKey = "schemaVersion";\n'
    'inline constexpr const char* kStateDataKey = "data";\n'
)

# The e05d2 editor-state + region-map vocabulary lives in editor_state_bridge.h as plain string
# constants (unlike the gesture verbs, which live in a switch), so the gate reads it the same way it
# reads the panel method names.
PANEL_CPP_EDITOR_STATE = (
    'inline constexpr const char* kEditorStateGetMethod = "editor.state.get";\n'
    'inline constexpr const char* kEditorStatePublishMethod = "editor.state.publish";\n'
    'inline constexpr const char* kEditorRegionsPublishMethod = "editor.regions.publish";\n'
    'inline constexpr const char* kEditorLayoutRestoredMethod = "editor.layout.restored";\n'
    'inline constexpr const char* kRegionKindViewport = "viewport";\n'
    'inline constexpr const char* kRegionKindNative = "native";\n'
)

# The e07c keybindings method lives in keybindings_bridge.h, read the same plain-constant way.
PANEL_CPP_KEYBINDINGS = 'inline constexpr const char* kKeybindingsGetMethod = "keybindings.get";\n'

# The e06b themes method lives in themes_bridge.h, read the same plain-constant way.
PANEL_CPP_THEMES = 'inline constexpr const char* kThemesGetMethod = "themes.get";\n'

PANEL_DOCUMENT = (
    "<!DOCTYPE html>\n"
    '<html lang="en">\n'
    '<head><meta charset="utf-8"><link rel="stylesheet" href="./app.css"></head>\n'
    '<body><main id="editor-root"></main>\n'
    '<script src="./dockview-core.min.js"></script>\n'
    '<script type="module" src="./editor-core.js"></script>\n'
    "</body>\n"
    "</html>\n"
)

PANEL_PACKAGE = {"name": "@context-engine/editor-core", "dependencies": {"dockview-core": "7.0.2"}}


def _panel_fixture(tmp_path: Path, *, bundle: str = PANEL_BUNDLE, document: str = PANEL_DOCUMENT,
                   header: str = PANEL_CPP_HEADER, state: str = PANEL_CPP_STATE,
                   source: str = PANEL_CPP_SOURCE, editor_state: str = PANEL_CPP_EDITOR_STATE,
                   keybindings: str = PANEL_CPP_KEYBINDINGS,
                   themes: str = PANEL_CPP_THEMES,
                   package: dict | None = None,
                   stage_dockview: bool = True) -> tuple[Path, Path, Path, Path, Path]:
    asset_dir = tmp_path / "app"
    asset_dir.mkdir(parents=True, exist_ok=True)
    (asset_dir / "editor-core.js").write_text(bundle, encoding="utf-8")
    (asset_dir / "index.html").write_text(document, encoding="utf-8")
    (asset_dir / "app.css").write_text(GOOD_STYLESHEET, encoding="utf-8")
    if stage_dockview:
        (asset_dir / "dockview-core.min.js").write_text("/* engine */\n", encoding="utf-8")

    include_dir = tmp_path / "shellinclude"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "panel_host.h").write_text(header, encoding="utf-8")
    (include_dir / "editor_state_bridge.h").write_text(editor_state, encoding="utf-8")
    (include_dir / "keybindings_bridge.h").write_text(keybindings, encoding="utf-8")
    (include_dir / "themes_bridge.h").write_text(themes, encoding="utf-8")

    contract_dir = tmp_path / "contractinclude"
    contract_dir.mkdir(parents=True, exist_ok=True)
    (contract_dir / "panel_state.h").write_text(state, encoding="utf-8")

    src_dir = tmp_path / "shellsrc"
    src_dir.mkdir(parents=True, exist_ok=True)
    (src_dir / "panel_host.cpp").write_text(source, encoding="utf-8")

    package_json = tmp_path / "package.json"
    package_json.write_text(
        json.dumps(PANEL_PACKAGE if package is None else package), encoding="utf-8")
    return asset_dir, include_dir, contract_dir, package_json, src_dir


def _run_panel(tmp_path: Path, **kwargs) -> int:
    asset_dir, include_dir, contract_dir, package_json, src_dir = _panel_fixture(tmp_path, **kwargs)
    return check_webui_assets.run_panel_contract(
        asset_dir, "editor-core.js", include_dir, contract_dir, package_json, src_dir)


def test_panel_contract_happy_path(tmp_path: Path, capsys) -> None:
    assert _run_panel(tmp_path) == 0
    assert "OK" in capsys.readouterr().out


@pytest.mark.parametrize("ts_name", [
    "PANEL_LIST_METHOD", "PANEL_RENDER_METHOD", "PANEL_COMMAND_METHOD",
    "PANEL_GESTURE_METHOD", "PANEL_STATE_GET_METHOD", "PANEL_STATE_SET_METHOD",
])
def test_panel_method_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """Every one of the six methods, not a sample: a gate that covers five is a gate with a hole."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1panel.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_a_panel_method_fails(tmp_path: Path) -> None:
    """An ABSENT constant is drift too — the runtime simply could not be calling that method."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "PANEL_RENDER_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


@pytest.mark.parametrize("ts_name", [
    "EDITOR_STATE_GET_METHOD", "EDITOR_STATE_PUBLISH_METHOD", "EDITOR_REGIONS_PUBLISH_METHOD",
    "EDITOR_LAYOUT_RESTORED_METHOD", "REGION_KIND_VIEWPORT", "REGION_KIND_NATIVE",
])
def test_editor_state_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e05d2 methods + region kinds, each one: a drift here breaks layout persistence silently."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1editor.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_an_editor_state_constant_fails(tmp_path: Path) -> None:
    """An ABSENT editor-state constant means editor-core cannot be calling the method the Shell routes."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "EDITOR_REGIONS_PUBLISH_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_editor_state_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING → exit 2."""
    renamed = PANEL_CPP_EDITOR_STATE.replace("kEditorStateGetMethod", "kEditorStateFetchMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, editor_state=renamed)


def test_keybindings_vocabulary_drift_fails(tmp_path: Path) -> None:
    """The e07c keybindings.get method: a drift here leaves the user override silently never loading."""
    drifted = re.sub(r'(KEYBINDINGS_GET_METHOD = ")[^"]*(")', r"\1keybindings.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_the_keybindings_constant_fails(tmp_path: Path) -> None:
    """An ABSENT keybindings constant means editor-core cannot be calling the method the Shell routes."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "KEYBINDINGS_GET_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_keybindings_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING → exit 2."""
    renamed = PANEL_CPP_KEYBINDINGS.replace("kKeybindingsGetMethod", "kKeybindingsFetchMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, keybindings=renamed)


def test_themes_vocabulary_drift_fails(tmp_path: Path) -> None:
    """The e06b themes.get method: a drift here leaves watched user themes silently never loading."""
    drifted = PANEL_BUNDLE.replace('THEMES_GET_METHOD = "themes.get"', 'THEMES_GET_METHOD = "themes.drifted"')
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_the_themes_constant_fails(tmp_path: Path) -> None:
    """An ABSENT themes constant means editor-core cannot be calling the method the Shell routes."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "THEMES_GET_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_themes_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING → exit 2."""
    renamed = PANEL_CPP_THEMES.replace("kThemesGetMethod", "kThemesFetchMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, themes=renamed)


@pytest.mark.parametrize("ts_name", ["STATE_SCHEMA_VERSION_KEY", "STATE_DATA_KEY"])
def test_d6_state_key_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """A drifted state key writes under one name and reads under another — every restore degrades."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("verb", ["begin", "extend", "commit", "cancel"])
def test_missing_gesture_verb_in_the_bundle_fails(tmp_path: Path, verb: str) -> None:
    """The closed vocabulary, both halves: a verb the runtime cannot name is a dead contract half."""
    stripped = PANEL_BUNDLE.replace(f'"{verb}"', '"__removed__"')
    assert _run_panel(tmp_path, bundle=stripped) == 1


def test_a_gesture_verb_vanishing_from_cpp_is_a_config_error(tmp_path: Path) -> None:
    """The dangerous direction: if the C++ vocabulary moved, the gate can verify NOTHING.

    Exit 2 (a config error), never a quiet pass — the same rot-into-a-no-op failure mode
    test_renamed_cpp_constant_is_a_config_error pins for the scheme gate. Mutating the SOURCE, not
    the header: the header carries no wire literals, so a header mutation is a no-op (which is
    exactly the vacuity this pair of tests exists to prevent).
    """
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, source=PANEL_CPP_SOURCE.replace('return "commit"', 'return "confirm"'))


def test_a_cpp_token_renamed_under_a_stale_ts_array_is_caught(tmp_path: Path) -> None:
    """THE REGRESSION THIS GATE EXISTS FOR — and the one the header-substring form could not see.

    C++ renames the wire token while the bundle keeps the old one. Both sides still "contain" the
    string somewhere, so a substring probe passes; a set comparison does not. The pinned vocabulary
    is checked FIRST, so this surfaces as the anti-rot CheckError rather than a silent OK.
    """
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, source=PANEL_CPP_SOURCE.replace('return "extend"', 'return "drag"'))


def test_a_verb_missing_from_the_bundle_array_is_a_drift_failure(tmp_path: Path) -> None:
    """The TS half: the C++ vocabulary is intact but the bundle's array lost a verb → exit 1."""
    stripped = PANEL_BUNDLE.replace(
        'var GESTURE_VERBS = ["begin", "extend", "commit", "cancel"];',
        'var GESTURE_VERBS = ["begin", "extend", "commit"];')
    assert _run_panel(tmp_path, bundle=stripped) == 1


def test_a_bundle_with_no_gesture_array_at_all_fails(tmp_path: Path) -> None:
    """Fail-closed: an absent array is a dead contract half, never a vacuous pass."""
    stripped = PANEL_BUNDLE.replace(
        'var GESTURE_VERBS = ["begin", "extend", "commit", "cancel"];', "")
    assert _run_panel(tmp_path, bundle=stripped) == 1


def test_missing_bundle_fails_the_panel_gate(tmp_path: Path) -> None:
    """Fail-closed on an absent build artifact (the scheme gate's sibling assertion)."""
    asset_dir, include_dir, contract_dir, package_json, src_dir = _panel_fixture(tmp_path)
    (asset_dir / "editor-core.js").unlink()
    assert check_webui_assets.run_panel_contract(
        asset_dir, "editor-core.js", include_dir, contract_dir, package_json, src_dir) == 1


def test_renamed_panel_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path,
                   header=PANEL_CPP_HEADER.replace("kPanelRenderMethod", "kPanelRenderMethodOld"))


def test_document_not_loading_the_docking_engine_fails(tmp_path: Path) -> None:
    """PanelHost reads the engine off the UMD global that script publishes — no tag, no panels."""
    without = PANEL_DOCUMENT.replace('<script src="./dockview-core.min.js"></script>\n', "")
    assert _run_panel(tmp_path, document=without) == 1


def test_unstaged_docking_engine_fails(tmp_path: Path) -> None:
    """A document that references the engine while the asset set does not carry it 404s at runtime."""
    assert _run_panel(tmp_path, stage_dockview=False) == 1


@pytest.mark.parametrize("dependencies", [
    # A version bump past the owner-ratified pin.
    {"dockview-core": "7.1.0"},
    # An ADDITIONAL dockview package — exactly what the s1 finding retired.
    {"dockview-core": "7.0.2", "dockview": "7.0.2"},
    # An unrelated dependency smuggled in.
    {"dockview-core": "7.0.2", "left-pad": "1.3.0"},
    # The dependency dropped entirely.
    {},
])
def test_dependency_drift_from_the_s1_approved_set_fails(tmp_path: Path, dependencies: dict) -> None:
    """08 section 3: a bump or an addition re-triggers the standing owner-consent gate."""
    package = dict(PANEL_PACKAGE)
    package["dependencies"] = dependencies
    assert _run_panel(tmp_path, package=package) == 1


def test_main_routes_the_panel_contract_flag(tmp_path: Path) -> None:
    asset_dir, include_dir, contract_dir, package_json, src_dir = _panel_fixture(tmp_path)
    assert check_webui_assets.main([
        "--asset-dir", str(asset_dir), "--bundle-name", "editor-core.js",
        "--panel-contract",
        "--shell-include-dir", str(include_dir),
        "--contract-include-dir", str(contract_dir),
        "--package-json", str(package_json),
        "--shell-src-dir", str(src_dir),
    ]) == 0


def test_missing_asset_dir_is_a_panel_config_error(tmp_path: Path) -> None:
    _, include_dir, contract_dir, package_json, src_dir = _panel_fixture(tmp_path)
    with pytest.raises(check_webui_assets.CheckError):
        check_webui_assets.run_panel_contract(
            tmp_path / "nope", "editor-core.js", include_dir, contract_dir, package_json, src_dir)


def test_the_real_repo_panel_sources_agree_across_languages() -> None:
    """Cross-check the COMMITTED panel sources, independent of any build (the scheme test's sibling).

    Catches a drift introduced without building — which is the common case during a refine pass.
    """
    shell_include = (REPO_ROOT / "src" / "editor" / "shell" / "include" / "context" / "editor" /
                     "shell")
    contract_include = (REPO_ROOT / "src" / "editor" / "gui" / "contract" / "include" / "context" /
                        "editor" / "gui" / "contract")
    ts = (REPO_ROOT / "src" / "editor" / "webui" / "core" / "src" / "panels.ts").read_text(
        encoding="utf-8")
    for _human, cpp_file, cpp_name, ts_name in check_webui_assets.PANEL_CONSTANTS:
        cpp_value = check_webui_assets._read_cpp_string_constant(shell_include / cpp_file, cpp_name)
        assert f'{ts_name} = "{cpp_value}"' in ts, f"{ts_name} drifted from C++ {cpp_name}"
    for _human, cpp_name, ts_name in check_webui_assets.PANEL_STATE_CONSTANTS:
        cpp_value = check_webui_assets._read_cpp_string_constant(
            contract_include / "panel_state.h", cpp_name)
        assert f'{ts_name} = "{cpp_value}"' in ts, f"{ts_name} drifted from C++ {cpp_name}"
    # The gesture vocabulary, read from BOTH real sources as SETS — the C++ wire tokens out of the
    # actual switch, the TS array out of the actual module. Asserting only `verb in ts` (the old
    # form) could not catch a .cpp token rename at all, because it never opened the .cpp.
    shell_src = REPO_ROOT / "src" / "editor" / "shell" / "src"
    cpp_verbs = check_webui_assets._read_cpp_gesture_verbs(shell_src / "panel_host.cpp")
    assert set(cpp_verbs) == set(check_webui_assets.GESTURE_VERBS), (
        f"the real C++ gesture vocabulary {sorted(cpp_verbs)} drifted from the pinned "
        f"{sorted(check_webui_assets.GESTURE_VERBS)}")
    ts_verbs = check_webui_assets._read_ts_string_array_from_bundle(ts, "GESTURE_VERBS")
    assert ts_verbs is not None, "panels.ts declares no GESTURE_VERBS array"
    assert set(ts_verbs) == set(cpp_verbs), (
        f"panels.ts GESTURE_VERBS {sorted(ts_verbs)} drifted from the C++ wire tokens "
        f"{sorted(cpp_verbs)}")


def test_the_real_editor_core_dependencies_are_the_approved_set() -> None:
    """The committed manifest, not a fixture: the pin is an owner ratification, not a default."""
    package = json.loads(
        (REPO_ROOT / "src" / "editor" / "webui" / "core" / "package.json").read_text(
            encoding="utf-8"))
    assert package["dependencies"] == check_webui_assets.EDITOR_CORE_DEPENDENCIES


# --------------------------------------------------------------------- check 6b: Dockview chrome
#
# The theme's `--dv-*` override must OUTRANK dockview-core's own injected stylesheet. These pin the
# regression that reddened `editor-cef-smoke-shell` on ubuntu + windows at M9 e06b: a bare
# `.dockview-theme-dark` block ties dockview's own specificity, loses on document order to the
# RUNTIME-injected copy, and the docking chrome silently keeps the engine's stock greys — with the
# live CEF smoke as the only signal, a full CI round-trip away.


def _css(tmp_path: Path, body: str) -> Path:
    sheet = tmp_path / "app.css"
    sheet.write_text(body, encoding="utf-8")
    return sheet


def test_an_unqualified_dockview_chrome_override_is_rejected(tmp_path: Path) -> None:
    sheet = _css(tmp_path, ".dockview-theme-dark {\n    --dv-background-color: #0a0a0a;\n}\n")
    failures = check_webui_assets.check_dockview_chrome_specificity(sheet)
    assert len(failures) == 1
    assert "same specificity" in failures[0].lower() or "SAME specificity" in failures[0]
    assert "html .dockview-theme-dark" in failures[0]


def test_a_qualified_dockview_chrome_override_passes(tmp_path: Path) -> None:
    sheet = _css(tmp_path, "html .dockview-theme-dark {\n    --dv-background-color: #0a0a0a;\n}\n")
    assert check_webui_assets.check_dockview_chrome_specificity(sheet) == []


def test_an_important_dockview_chrome_override_passes(tmp_path: Path) -> None:
    """`!important` is the other way to win the cascade; the gate accepts either instrument."""
    sheet = _css(tmp_path, ".dockview-theme-dark {\n    --dv-background-color: #0a0a0a !important;\n}\n")
    assert check_webui_assets.check_dockview_chrome_specificity(sheet) == []


def test_a_dockview_block_that_sets_no_dv_variable_is_not_the_gate_s_business(tmp_path: Path) -> None:
    """Only variable DECLARATIONS can lose this cascade; an ordinary rule is left alone."""
    sheet = _css(tmp_path, ".dockview-theme-dark {\n    padding: 0;\n}\n--dv-marker: in-a-comment;\n")
    assert check_webui_assets.check_dockview_chrome_specificity(sheet) == []


def test_the_real_app_css_dockview_override_outranks_the_vendored_engine() -> None:
    """Ground truth, not a fixture: the SHIPPED stylesheet must win the cascade."""
    sheet = REPO_ROOT / "src" / "editor" / "webui" / "app" / "app.css"
    assert check_webui_assets.check_dockview_chrome_specificity(sheet) == []
