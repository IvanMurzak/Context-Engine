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

# The served-font halves (2026-08-27): app.css declares the two families the themes lead with, and
# the staged fonts/ dir holds the bytes the declarations pull. The gate holds them in lockstep.
GOOD_FONT_STYLESHEET = """\
@font-face {
    font-family: 'Geist';
    src: url('fonts/Geist-Variable.woff2') format('woff2');
    font-weight: 100 900;
}
@font-face {
    font-family: 'Geist Mono';
    src: url('fonts/GeistMono-Variable.woff2') format('woff2');
    font-weight: 100 900;
}
"""
FONT_FILES = ("Geist-Variable.woff2", "GeistMono-Variable.woff2")


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
            js: bytes = DOCKVIEW_JS, css: bytes = DOCKVIEW_CSS,
            stylesheet: str | None = GOOD_FONT_STYLESHEET,
            fonts: tuple[str, ...] = FONT_FILES) -> Path:
    asset_dir = tmp_path / "app"
    asset_dir.mkdir(parents=True, exist_ok=True)
    if bundle is not None:
        (asset_dir / "editor-core.js").write_text(bundle, encoding="utf-8")
    (asset_dir / "dockview-core.min.js").write_bytes(js)
    (asset_dir / "dockview.css").write_bytes(css)
    if stylesheet is not None:
        (asset_dir / "app.css").write_text(stylesheet, encoding="utf-8")
    if fonts:
        (asset_dir / "fonts").mkdir(exist_ok=True)
        for font in fonts:
            (asset_dir / "fonts" / font).write_bytes(b"wOF2fake-bytes")
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


# --- served font faces (2026-08-27) ---------------------------------------------------------------
# Each case plants ONE half of the drift that actually shipped: e06a's faces existed in the repo
# while nothing staged or declared them, and the editor silently rendered in the platform sans.

def test_missing_staged_font_fails(tmp_path: Path, capsys) -> None:
    """@font-face declared, bytes never staged — the browser falls back with no error anywhere."""
    asset_dir = _assets(tmp_path, fonts=("Geist-Variable.woff2",))  # the Mono bytes are missing
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1
    assert "GeistMono-Variable.woff2" in capsys.readouterr().err


def test_empty_staged_font_fails(tmp_path: Path) -> None:
    """A zero-byte font is a fetch that 'succeeds' and a face that never renders."""
    asset_dir = _assets(tmp_path)
    (asset_dir / "fonts" / "Geist-Variable.woff2").write_bytes(b"")
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


def test_missing_font_face_declaration_fails(tmp_path: Path, capsys) -> None:
    """Bytes staged, family never declared — exactly as silent as the missing-bytes half."""
    no_mono = GOOD_FONT_STYLESHEET[:GOOD_FONT_STYLESHEET.rindex("@font-face")]
    asset_dir = _assets(tmp_path, stylesheet=no_mono)
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1
    assert "Geist Mono" in capsys.readouterr().err


def test_font_face_url_drift_fails(tmp_path: Path, capsys) -> None:
    """The css and the staging list live in different files; a rename on one side must red."""
    drifted = GOOD_FONT_STYLESHEET.replace("fonts/Geist-Variable.woff2", "fonts/Geist.woff2")
    # The drifted target IS staged, so the only failure left is the lockstep drift itself.
    asset_dir = _assets(tmp_path, stylesheet=drifted, fonts=FONT_FILES + ("Geist.woff2",))
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1
    assert "drift" in capsys.readouterr().err


def test_missing_app_stylesheet_fails_the_asset_gate(tmp_path: Path) -> None:
    """app.css is part of the served set; without it no font face exists at all."""
    asset_dir = _assets(tmp_path, stylesheet=None)
    assert check_webui_assets.run(asset_dir, _manifest(tmp_path), "editor-core.js") == 1


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

# The constants the bundle must agree with the Shell about. Since M9 e13a-2 that includes the
# EXTENSION scheme (`context-ext://`), which editor-core uses to build a third-party panel's
# `<iframe src>` — a rename there leaves every package panel framing a scheme the Shell does not
# serve, blank, with no build error anywhere. Since M9 e13b-1 it also includes the PANEL-PORT
# HANDSHAKE TAG: the Shell BUILDS the injected bootstrap script's bytes from its copy and editor-core
# refuses any handshake that does not carry it, so a drift makes every package panel portless — the
# same silence, one layer up.
SCHEME_BUNDLE = (
    'var BRIDGE_SCHEME = "context-editor";\n'
    'var BRIDGE_ORIGIN = "context-editor://app";\n'
    'var BRIDGE_ENDPOINT = "context-editor://ipc";\n'
    'var BRIDGE_QUERY_FUNCTION = "contextEditorQuery";\n'
    'var BRIDGE_CANCEL_FUNCTION = "contextEditorQueryCancel";\n'
    'var THEME_PIN_FLAG = "ctx-smoke-theme";\n'
    'var EXT_SCHEME = "context-ext";\n'
    'var EXT_URL_PREFIX = "context-ext://";\n'
    'var EXT_PORT_HANDSHAKE_TAG = "context.panel-port.v1";\n'
)

CPP_HEADER = (
    'inline constexpr const char* kAppScheme = "context-editor";\n'
    'inline constexpr const char* kAppOrigin = "context-editor://app";\n'
    'inline constexpr const char* kIpcEndpoint = "context-editor://ipc";\n'
    'inline constexpr const char* kThemePinFlag = "ctx-smoke-theme";\n'
)

# The e13a-2 extension scheme lives in its own header next to app_scheme.h, so the fixture writes a
# second file — which is also what pins that SCHEME_CONSTANTS' per-entry `cpp_file` is honoured
# rather than every constant being read out of one hardcoded header.
CPP_EXT_HEADER = (
    'inline constexpr const char* kExtScheme = "context-ext";\n'
    'inline constexpr const char* kExtUrlPrefix = "context-ext://";\n'
    'inline constexpr const char* kExtPortHandshakeTag = "context.panel-port.v1";\n'
)

CPP_CEF = (
    'constexpr const char* kBridgeQueryFunction = "contextEditorQuery";\n'
    'constexpr const char* kBridgeCancelFunction = "contextEditorQueryCancel";\n'
)


def _scheme_fixture(tmp_path: Path, *, document: str = GOOD_DOCUMENT,
                    stylesheet: str | None = GOOD_STYLESHEET, bundle: str = SCHEME_BUNDLE,
                    header: str = CPP_HEADER, ext_header: str = CPP_EXT_HEADER,
                    cef: str = CPP_CEF) -> tuple[Path, Path, Path]:
    asset_dir = tmp_path / "app"
    asset_dir.mkdir(parents=True, exist_ok=True)
    (asset_dir / "editor-core.js").write_text(bundle, encoding="utf-8")
    (asset_dir / "index.html").write_text(document, encoding="utf-8")
    if stylesheet is not None:
        (asset_dir / "app.css").write_text(stylesheet, encoding="utf-8")

    include_dir = tmp_path / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "app_scheme.h").write_text(header, encoding="utf-8")
    (include_dir / "ext_scheme.h").write_text(ext_header, encoding="utf-8")

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
    # M9 e13a-2 — the EXTENSION scheme. editor-core builds a third-party panel's `<iframe src>` from
    # these, so a drift here does not break a bridge, it points every package panel at a scheme
    # nothing serves. Both spellings are covered because they are separate constants that a
    # half-applied rename would leave DISAGREEING WITH EACH OTHER while each still looked plausible.
    ("EXT_SCHEME", "context-extension"),
    ("EXT_URL_PREFIX", "context-ext:/"),
    # M9 e13b-1 — the PANEL-PORT HANDSHAKE TAG. A drift here breaks neither the bridge nor the frame
    # URL: the panel loads, parses and runs, and only the PORT never arrives, so every future
    # capability verb is unreachable with nothing naming the cause. The drifted value below is a
    # VERSION BUMP rather than a typo, deliberately — the tag carries its own protocol version, so
    # `…v2` is the realistic half-applied rename (one side bumped, the other not) and it must fail
    # exactly as a misspelling does.
    ("EXT_PORT_HANDSHAKE_TAG", "context.panel-port.v2"),
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
    # The WHOLE editor-core source, not `bridge.ts` alone: the scheme vocabulary is no longer
    # confined to one module (`THEME_PIN_FLAG` is theme.ts's), and pinning the search to a single
    # file would turn "the constant moved house" into a spurious drift failure.
    ts = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((REPO_ROOT / "src" / "editor" / "webui" / "core" / "src").glob("*.ts")))
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
    # editor-window-chrome a1: the titlebar's four caption chrome regions.
    'var REGION_KIND_CAPTION = "caption";\n'
    'var REGION_KIND_CAPTION_MIN = "caption-min";\n'
    'var REGION_KIND_CAPTION_MAX = "caption-max";\n'
    'var REGION_KIND_CAPTION_CLOSE = "caption-close";\n'
    # e07c keybindings vocabulary (keymap.ts).
    'var KEYBINDINGS_GET_METHOD = "keybindings.get";\n'
    # e06b themes vocabulary (theme.ts).
    'var THEMES_GET_METHOD = "themes.get";\n'
    # e06d user-config vocabulary (config.ts).
    'var CONFIG_GET_METHOD = "config.get";\n'
    'var CONFIG_SET_METHOD = "config.set";\n'
    'var CONFIG_THEME_KEY = "theme";\n'
    # e08d session-relay vocabulary (session.ts + when.ts).
    'var SESSION_STATE_METHOD = "session.state";\n'
    'var PLAY_STATE_EVENT = "play-state";\n'
    # editor-window-chrome d1: the session.control write half + its verbs (session.ts).
    'var SESSION_CONTROL_METHOD = "session.control";\n'
    'var SESSION_CONTROL_VERB_PLAY = "play";\n'
    'var SESSION_CONTROL_VERB_PAUSE = "pause";\n'
    'var SESSION_CONTROL_VERB_STOP = "stop";\n'
    'var SESSION_CONTROL_VERB_STEP = "step";\n'
    # e10b window-management vocabulary (window.ts).
    'var WINDOW_LIST_METHOD = "window.list";\n'
    'var WINDOW_TEAR_OUT_METHOD = "window.tear-out";\n'
    'var WINDOW_MOVE_TO_METHOD = "window.move-to";\n'
    'var WINDOW_SEED_METHOD = "window.seed";\n'
    'var WINDOW_REHOMED_METHOD = "window.rehomed";\n'
    'var WINDOW_CLOSE_METHOD = "window.close";\n'
    # editor-window-chrome a1: the three window-control verbs + the chrome contract (window.ts).
    'var WINDOW_MINIMIZE_METHOD = "window.minimize";\n'
    'var WINDOW_TOGGLE_MAXIMIZE_METHOD = "window.toggle-maximize";\n'
    'var WINDOW_FOCUS_METHOD = "window.focus";\n'
    # editor-window-chrome b1: the appearance report + its fail-closed tokens (window.ts).
    'var WINDOW_SET_APPEARANCE_METHOD = "window.set-appearance";\n'
    'var WINDOW_APPEARANCE_DARK = "dark";\n'
    'var WINDOW_APPEARANCE_LIGHT = "light";\n'
    'var CHROME_STATE_METHOD = "chrome.state";\n'
    'var CHROME_MODE_CUSTOM = "custom";\n'
    'var CHROME_MODE_HYBRID = "hybrid";\n'
    'var CHROME_MODE_SYSTEM = "system";\n'
    'var CHROME_WINDOW_PRIMARY = "primary";\n'
    'var CHROME_WINDOW_SECONDARY = "secondary";\n'
    # editor-window-chrome a1: the `editor.ui.chrome` fact topic (uibus.ts).
    'var UI_TOPIC_CHROME = "editor.ui.chrome";\n'
    # e10c cross-window drag vocabulary (drag.ts).
    'var DRAG_PROBE_METHOD = "drag.probe";\n'
    'var DRAG_REPORT_ZONE_METHOD = "drag.report-zone";\n'
    # e10d cross-window editor.ui mirror vocabulary (uimirror.ts).
    'var UI_MIRROR_METHOD = "ui.mirror";\n'
    'var UI_MIRROR_POLL_METHOD = "ui.mirror-poll";\n'
    'var UI_MIRROR_REPORT_METHOD = "ui.mirror-report";\n'
    # e09b-3 LOUD write-notice vocabulary (uibus.ts topic + notifications.ts kinds).
    'var UI_TOPIC_WRITE_NOTICE = "editor.ui.write-notice";\n'
    'var WRITE_NOTICE_KIND_DROP = "drop";\n'
    'var WRITE_NOTICE_KIND_REFUSAL = "refusal";\n'
    # x10 abandoned-gesture kind (notifications.ts).
    'var WRITE_NOTICE_KIND_ABANDONED = "abandoned";\n'
    # e13c-1 package daemon fan-in method (boot.ts).
    'var PANEL_DAEMON_CALL_METHOD = "panel.daemon.call";\n'
    # e13c-2 package daemon event fan-OUT drain method (packageevents.ts).
    'var PANEL_EVENTS_POLL_METHOD = "panel.events.poll";\n'
    # e13c-4 install-consent READ method (packagegrants.ts). The DECIDE half is deliberately absent:
    # it has no production caller yet, so esbuild tree-shakes it out of the real bundle and the gate
    # does not enroll it — a fixture that carried it would be MORE CAPABLE THAN THE REAL BUNDLE.
    'var PACKAGE_GRANTS_LIST_METHOD = "package.grants.list";\n'
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
    'inline constexpr const char* kRegionKindCaption = "caption";\n'
    'inline constexpr const char* kRegionKindCaptionMin = "caption-min";\n'
    'inline constexpr const char* kRegionKindCaptionMax = "caption-max";\n'
    'inline constexpr const char* kRegionKindCaptionClose = "caption-close";\n'
)

# The e07c keybindings method lives in keybindings_bridge.h, read the same plain-constant way.
PANEL_CPP_KEYBINDINGS = 'inline constexpr const char* kKeybindingsGetMethod = "keybindings.get";\n'

# The e06b themes method lives in themes_bridge.h, read the same plain-constant way.
PANEL_CPP_THEMES = 'inline constexpr const char* kThemesGetMethod = "themes.get";\n'

# The e06d user-config methods + the one settable key live in user_config.h, same plain-constant way.
PANEL_CPP_CONFIG = (
    'inline constexpr const char* kConfigGetMethod = "config.get";\n'
    'inline constexpr const char* kConfigSetMethod = "config.set";\n'
    'inline constexpr const char* kConfigThemeKey = "theme";\n'
)

# The e08d session relay's method + fact discriminator live in session_bridge.h, same
# plain-constant way. BOTH are pinned because they fail DIFFERENTLY and both failures are
# silent: a renamed METHOD means editor-core calls something the Shell no longer routes, a
# renamed EVENT means it receives a reply `applyFact` no longer recognises — and either one
# reproduces the frozen `playState` e08d fixed.
PANEL_CPP_SESSION = (
    'inline constexpr const char* kSessionStateMethod = "session.state";\n'
    'inline constexpr const char* kSessionPlayStateEvent = "play-state";\n'
    # editor-window-chrome d1: the write half. The verbs are pinned beside the method because
    # a drifted verb fails SOFTER but just as silently -- the Shell answers `session.bad_verb`
    # and the strip's press does nothing, with both builds green.
    'inline constexpr const char* kSessionControlMethod = "session.control";\n'
    'inline constexpr const char* kSessionControlVerbPlay = "play";\n'
    'inline constexpr const char* kSessionControlVerbPause = "pause";\n'
    'inline constexpr const char* kSessionControlVerbStop = "stop";\n'
    'inline constexpr const char* kSessionControlVerbStep = "step";\n'
)

# The e10b window-management surface's methods live in window_bridge.h, same plain-constant way. A
# rename on either side leaves a tear-out / move / rehome calling a method the Shell no longer routes,
# so the panel is silently lost — the exact thing 03 §7's loud-degradation contract forbids.
PANEL_CPP_WINDOW = (
    'inline constexpr const char* kWindowListMethod = "window.list";\n'
    'inline constexpr const char* kWindowTearOutMethod = "window.tear-out";\n'
    'inline constexpr const char* kWindowMoveToMethod = "window.move-to";\n'
    'inline constexpr const char* kWindowSeedMethod = "window.seed";\n'
    'inline constexpr const char* kWindowRehomedMethod = "window.rehomed";\n'
    'inline constexpr const char* kWindowCloseMethod = "window.close";\n'
    # e10c: the cross-window drag surface lives on the same header (window_bridge.h).
    'inline constexpr const char* kDragProbeMethod = "drag.probe";\n'
    'inline constexpr const char* kDragReportZoneMethod = "drag.report-zone";\n'
    # e10d: the cross-window editor.ui mirror surface lives on the same header (window_bridge.h).
    'inline constexpr const char* kUiMirrorMethod = "ui.mirror";\n'
    'inline constexpr const char* kUiMirrorPollMethod = "ui.mirror-poll";\n'
    'inline constexpr const char* kUiMirrorReportMethod = "ui.mirror-report";\n'
    # editor-window-chrome a1: the three control verbs + the chrome contract ride window_bridge.h.
    'inline constexpr const char* kWindowMinimizeMethod = "window.minimize";\n'
    'inline constexpr const char* kWindowToggleMaximizeMethod = "window.toggle-maximize";\n'
    'inline constexpr const char* kWindowFocusMethod = "window.focus";\n'
    # editor-window-chrome b1: the appearance report + its fail-closed tokens ride window_bridge.h.
    'inline constexpr const char* kWindowSetAppearanceMethod = "window.set-appearance";\n'
    'inline constexpr const char* kWindowAppearanceDark = "dark";\n'
    'inline constexpr const char* kWindowAppearanceLight = "light";\n'
    'inline constexpr const char* kChromeStateMethod = "chrome.state";\n'
    'inline constexpr const char* kChromeModeCustom = "custom";\n'
    'inline constexpr const char* kChromeModeHybrid = "hybrid";\n'
    'inline constexpr const char* kChromeModeSystem = "system";\n'
    'inline constexpr const char* kChromeWindowPrimary = "primary";\n'
    'inline constexpr const char* kChromeWindowSecondary = "secondary";\n'
)

# The a1 `editor.ui.chrome` fact topic lives in its OWN header (chrome_facts.h) — the unicast
# maximized-fact relay's home, not window_bridge.h — so the fixture mirrors that split.
PANEL_CPP_CHROME_FACTS = 'inline constexpr const char* kUiTopicChrome = "editor.ui.chrome";\n'

# The e09b-3 LOUD write-notice vocabulary lives in its OWN header (write_notice.h), unlike the drag /
# mirror surfaces that ride window_bridge.h — it is a write-path concern, not a window-management one.
# Neither of its drifts produces an `unknown_method`, which is what makes pinning it worth a test: a
# topic drift makes editor-core's CLOSED bus refuse every notice (a refused write goes back to being
# invisible), and a kind drift mis-hues it (the human is told the wrong thing).
PANEL_CPP_WRITE_NOTICE = (
    'inline constexpr const char* kUiTopicWriteNotice = "editor.ui.write-notice";\n'
    'inline constexpr const char* kWriteNoticeOrigin = "shell";\n'
    'inline constexpr const char* kWriteNoticeKindDrop = "drop";\n'
    'inline constexpr const char* kWriteNoticeKindRefusal = "refusal";\n'
    'inline constexpr const char* kWriteNoticeKindAbandoned = "abandoned";\n'
)

# The e13c-1 package daemon fan-in lives in its OWN header (package_sessions.h), like the write-notice
# vocabulary above and unlike the surfaces that ride window_bridge.h. It is the ONLY Shell method that
# forwards a package panel's call onto a daemon wire, so its drift is the widest of the family: every
# package panel's `bridge.call` refuses with a code indistinguishable from "this build never
# implemented it", on a green build and a green suite on both sides.
PANEL_CPP_PACKAGE_SESSIONS = (
    'inline constexpr const char* kPanelDaemonCallMethod = "panel.daemon.call";\n'
    'inline constexpr const char* kPackageSessionScope = "read";\n'
)

# The e13c-2 event fan-OUT drain lives in package_events.h, NOT beside the fan-in above — mirroring
# the real split (the buffer's header owns the method it is polled through). Its drift symptom is the
# quietest of the family: editor-core keeps polling on its tick, `PackageEventPump` swallows the
# refusal by design, and a package's events simply stop arriving with nothing reporting it.
PANEL_CPP_PACKAGE_EVENTS = (
    'inline constexpr const char* kPanelEventsPollMethod = "panel.events.poll";\n'
    'inline constexpr std::size_t kMaxBufferedEventsPerPackage = 256;\n'
)

PANEL_CPP_PACKAGE_GRANTS = (
    'inline constexpr const char* kPackageGrantsListMethod = "package.grants.list";\n'
    'inline constexpr const char* kPackageGrantsDecideMethod = "package.grants.decide";\n'
)

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
                   config: str = PANEL_CPP_CONFIG,
                   session: str = PANEL_CPP_SESSION,
                   window: str = PANEL_CPP_WINDOW,
                   chrome_facts: str = PANEL_CPP_CHROME_FACTS,
                   write_notice: str = PANEL_CPP_WRITE_NOTICE,
                   package_sessions: str = PANEL_CPP_PACKAGE_SESSIONS,
                   package_events: str = PANEL_CPP_PACKAGE_EVENTS,
                   package_grants: str = PANEL_CPP_PACKAGE_GRANTS,
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
    (include_dir / "user_config.h").write_text(config, encoding="utf-8")
    (include_dir / "session_bridge.h").write_text(session, encoding="utf-8")
    (include_dir / "window_bridge.h").write_text(window, encoding="utf-8")
    (include_dir / "chrome_facts.h").write_text(chrome_facts, encoding="utf-8")
    (include_dir / "write_notice.h").write_text(write_notice, encoding="utf-8")
    (include_dir / "package_sessions.h").write_text(package_sessions, encoding="utf-8")
    (include_dir / "package_events.h").write_text(package_events, encoding="utf-8")
    (include_dir / "package_grants.h").write_text(package_grants, encoding="utf-8")

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
    "REGION_KIND_CAPTION", "REGION_KIND_CAPTION_MIN", "REGION_KIND_CAPTION_MAX",
    "REGION_KIND_CAPTION_CLOSE",
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

# The bundle-side session vocabulary (e08d + the d1 control surface) — ONE list for both
# parametrized session gates below, so a new verb is one edit, not the two-list drift this
# file exists to prevent.
SESSION_TS_NAMES = (
    "SESSION_STATE_METHOD", "PLAY_STATE_EVENT", "SESSION_CONTROL_METHOD",
    "SESSION_CONTROL_VERB_PLAY", "SESSION_CONTROL_VERB_PAUSE", "SESSION_CONTROL_VERB_STOP",
    "SESSION_CONTROL_VERB_STEP",
)


@pytest.mark.parametrize("ts_name", SESSION_TS_NAMES)
def test_session_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e08d session relay: a drift here re-freezes editor-core's `playState` at its baseline.

    Worth its own case per member because the two drift INDEPENDENTLY and neither is visible at
    runtime: a renamed METHOD is a refusal boot.ts degrades over, a renamed EVENT is a reply
    `DaemonSessionState.applyFact` silently ignores. Both leave the editor up, nothing erroring, and
    every `playState == playing` clause wrong — the exact state e08b shipped and e08d removed.
    """
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("ts_name", SESSION_TS_NAMES)
def test_bundle_missing_a_session_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT session constant means editor-core is not on the relay the Shell serves at all."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_session_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_SESSION.replace("kSessionStateMethod", "kSessionFetchMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, session=renamed)


@pytest.mark.parametrize(
    "ts_name",
    ["WINDOW_LIST_METHOD", "WINDOW_TEAR_OUT_METHOD", "WINDOW_MOVE_TO_METHOD",
     "WINDOW_SEED_METHOD", "WINDOW_REHOMED_METHOD", "WINDOW_CLOSE_METHOD",
     "WINDOW_MINIMIZE_METHOD", "WINDOW_TOGGLE_MAXIMIZE_METHOD", "WINDOW_FOCUS_METHOD"])
def test_window_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e10b window surface: a drift here leaves a tear-out / move / rehome calling a method the
    Shell no longer routes, so the panel refuses with `unknown_method` and is silently lost — the exact
    03 §7 failure the DoD forbids, invisible at runtime (the editor is up, nothing errors)."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1window.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("ts_name", ["WINDOW_TEAR_OUT_METHOD", "WINDOW_SEED_METHOD"])
def test_bundle_missing_a_window_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT window constant means editor-core cannot be calling the surface the Shell serves."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_window_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_WINDOW.replace("kWindowTearOutMethod", "kWindowRipOutMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, window=renamed)


@pytest.mark.parametrize("ts_name", ["DRAG_PROBE_METHOD", "DRAG_REPORT_ZONE_METHOD"])
def test_drag_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e10c cross-window drag surface: a drift here leaves the target window's editor-core calling a
    method the Shell no longer routes, so its drop-zone answer never round-trips and a cross-window drop
    silently does nothing — invisible at runtime, red one live-CEF-smoke round away."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1drag.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("ts_name", ["DRAG_PROBE_METHOD", "DRAG_REPORT_ZONE_METHOD"])
def test_bundle_missing_a_drag_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT drag constant means editor-core cannot be answering the cross-window drop-zone query."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_drag_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ drag constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_WINDOW.replace("kDragProbeMethod", "kDragPollMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, window=renamed)


@pytest.mark.parametrize(
    "ts_name", ["UI_MIRROR_METHOD", "UI_MIRROR_POLL_METHOD", "UI_MIRROR_REPORT_METHOD"])
def test_ui_mirror_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e10d cross-window mirror surface: a drift here leaves the sink / poller / reporter calling a
    method the Shell no longer routes, so an editor.ui fact published in one window silently never reaches
    the others — the mirror stops propagating with the editor up and nothing erroring."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1ui.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("ts_name", ["UI_MIRROR_METHOD", "UI_MIRROR_REPORT_METHOD"])
def test_bundle_missing_a_ui_mirror_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT mirror constant means editor-core cannot be on the cross-window surface the Shell serves."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_ui_mirror_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ mirror constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_WINDOW.replace("kUiMirrorReportMethod", "kUiMirrorAckMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, window=renamed)


@pytest.mark.parametrize("ts_name", [
    "CHROME_STATE_METHOD", "CHROME_MODE_CUSTOM", "CHROME_MODE_HYBRID", "CHROME_MODE_SYSTEM",
    "CHROME_WINDOW_PRIMARY", "CHROME_WINDOW_SECONDARY", "UI_TOPIC_CHROME",
])
def test_chrome_contract_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The a1 chrome contract, each member: the tokens fail WORSE than the method — nothing refuses,
    a drifted mode parses as the `system` fallback so the strip silently renders the wrong chrome,
    and a drifted topic silently freezes the a2 max/restore glyph."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1chrome.drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize("ts_name", ["CHROME_STATE_METHOD", "UI_TOPIC_CHROME"])
def test_bundle_missing_a_chrome_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT chrome constant means editor-core cannot be reading the contract the Shell serves."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_chrome_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_CHROME_FACTS.replace("kUiTopicChrome", "kUiTopicChromeFacts")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, chrome_facts=renamed)


# --- e13c-1: the package daemon fan-in method ----------------------------------------------------
#
# The widest-blast-radius pair in this file. `panel.daemon.call` is the ONLY Shell router method that
# carries a third-party package panel's call onto a daemon wire, and a drift does not degrade the
# feature — it removes it entirely, while both languages build green and both suites stay green,
# because the C++ side is pinned to the literal by its own test whereas every TS reader goes through
# the constant. The refusal a drifted build produces (`verb_not_granted`) is the SAME one a build that
# never implemented `bridge.call` would produce, so there is no signal anywhere.


def test_package_session_method_drift_fails(tmp_path: Path) -> None:
    """The e13c-1 fan-in: editor-core calling a method the Shell no longer routes means EVERY package
    panel's `bridge.call` refuses, indistinguishably from the verb not existing."""
    drifted = re.sub(r'(PANEL_DAEMON_CALL_METHOD = ")[^"]*(")', r"\1panel.daemon.drifted\2",
                     PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_the_package_session_constant_fails(tmp_path: Path) -> None:
    """An ABSENT constant means editor-core cannot be calling the fan-in at all, so no package panel
    could reach its own baseline daemon session."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "PANEL_DAEMON_CALL_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_package_session_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_PACKAGE_SESSIONS.replace("kPanelDaemonCallMethod", "kPanelDaemonInvokeMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, package_sessions=renamed)


# --- e13c-2: the event fan-OUT drain method ------------------------------------------------------
#
# The SAME three cases as the fan-in above, because a new tuple in PACKAGE_SESSION_CONSTANTS that
# nobody drifts is a gate entry whose own non-vacuity is unproven — the happy path passes just as
# readily with the entry deleted. Its drift symptom is the quietest in the family: an idle package
# panel and a permanently broken one are indistinguishable from every observable.


def test_package_events_poll_method_drift_fails(tmp_path: Path) -> None:
    """Drift the TS side: editor-core polls a drain the Shell does not route, `PackageEventPump`
    swallows the refusal by design, and a package's events stop arriving with NOTHING reporting it."""
    drifted = re.sub(r'(PANEL_EVENTS_POLL_METHOD = ")[^"]*(")', r"\1panel.events.drifted\2",
                     PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_the_package_events_constant_fails(tmp_path: Path) -> None:
    """An ABSENT constant means editor-core never drains the buffer, so the Shell fills it to its cap
    and drop-oldests forever while every panel renders the past."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "PANEL_EVENTS_POLL_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_package_events_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard, and the reason the drain is read from package_events.h rather than
    package_sessions.h: rename the C++ constant and the gate can verify NOTHING -> exit 2."""
    renamed = PANEL_CPP_PACKAGE_EVENTS.replace("kPanelEventsPollMethod", "kPanelEventsDrainMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, package_events=renamed)


# --- e13c-4: the install-consent READ method -----------------------------------------------------
#
# The same three shapes as the two rows above, for the same reason and with the WORST symptom of the
# family: `ShellPackageGrants.load` is fail-closed by design, so a drift here does not error - every
# package silently falls to the deny-all floor and holds no capability at all, which is
# indistinguishable from an editor on which the operator has simply granted nothing.


def test_a_drifted_package_grants_list_method_fails(tmp_path: Path) -> None:
    """The Shell would route one name and editor-core would call another."""
    drifted = re.sub(r'(PACKAGE_GRANTS_LIST_METHOD = ")[^"]*(")', r"\1package.grants.drifted\2",
                     PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


def test_bundle_missing_the_package_grants_constant_fails(tmp_path: Path) -> None:
    """An ABSENT constant means editor-core never reads the consent surface, so every package sits on
    the deny-all floor with nothing reporting it."""
    stripped = "\n".join(
        line for line in PANEL_BUNDLE.splitlines() if "PACKAGE_GRANTS_LIST_METHOD" not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_package_grants_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2,
    rather than reporting a green it did not earn."""
    renamed = PANEL_CPP_PACKAGE_GRANTS.replace("kPackageGrantsListMethod",
                                               "kPackageGrantsReadMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, package_grants=renamed)


# --- e09b-3: the LOUD write-notice vocabulary ----------------------------------------------------
#
# Each case below was verified by PLANTING the drift against the REAL tree first (the P8/P9 plants of
# this task's anti-vacuity round) and watching `webui-panel-contract` go red; they are kept here as
# regressions so the gate cannot rot back into a no-op.


@pytest.mark.parametrize(
    "ts_name",
    ["UI_TOPIC_WRITE_NOTICE", "WRITE_NOTICE_KIND_DROP", "WRITE_NOTICE_KIND_REFUSAL",
     "WRITE_NOTICE_KIND_ABANDONED"])
def test_write_notice_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """A drift here is SILENT in a way the sibling method surfaces are not — no `unknown_method` is
    ever produced. The topic drifting makes editor-core's CLOSED bus refuse every notice, so a refused
    write becomes invisible to the human again (the exact defect e09b-3 exists to end); a KIND drifting
    mis-hues it, telling the human their project is unreachable when a colleague merely edited the same
    field. Both leave a green build on both sides of the wire."""
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize(
    "ts_name",
    ["UI_TOPIC_WRITE_NOTICE", "WRITE_NOTICE_KIND_DROP", "WRITE_NOTICE_KIND_REFUSAL",
     "WRITE_NOTICE_KIND_ABANDONED"])
def test_bundle_missing_a_write_notice_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT constant means editor-core is not rendering the notices the Shell publishes at all —
    i.e. the notification host was tree-shaken out or never wired, which looks like a quiet editor.

    `WRITE_NOTICE_KIND_REFUSAL` is the one worth naming explicitly: no PRODUCTION line references it
    (`writeNoticeTone`/`writeNoticeHeadline` both branch on `_DROP` and let everything else fall
    through), so it survives into the bundle solely because `index.ts` re-exports the module. That is
    exactly the shape esbuild tree-shakes away — the reason `WELCOME_MODE_PROJECT` and
    `DAEMON_OWNERSHIP_EXTERNAL` are deliberately EXCLUDED from their own pin tables above — so this
    case is what keeps the barrel export from being dropped as dead weight without anyone noticing."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_write_notice_constant_named_in_a_COMMENT_does_not_satisfy_the_pin(
        tmp_path: Path) -> None:
    """A comment restating the OLD value must not shadow a drifted declaration.

    `write_notice.h` documents its own vocabulary at length, and the C++ reader takes `re.search`'s
    FIRST match — so without comment-stripping a line like `// kWriteNoticeKindDrop = "drop" was the
    original spelling.` left above a renamed value would be compared against TS instead of the real
    declaration, and the gate would report OK across a live drift. Verifying prose instead of code is
    the one outcome a cross-language pin must never have."""
    shadowed = PANEL_CPP_WRITE_NOTICE.replace(
        'inline constexpr const char* kWriteNoticeKindDrop = "drop";',
        '// historical note: kWriteNoticeKindDrop = "drop" was the original spelling.\n'
        'inline constexpr const char* kWriteNoticeKindDrop = "dropped";')
    assert shadowed != PANEL_CPP_WRITE_NOTICE
    assert _run_panel(tmp_path, write_notice=shadowed) == 1


def test_a_renamed_write_notice_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard: rename the C++ constant and the gate can verify NOTHING -> exit 2.

    This is the failure the gate must NOT swallow: with the anchor gone the regex matches nothing, and
    a check that silently verifies nothing is worse than no check, because it keeps reporting OK."""
    renamed = PANEL_CPP_WRITE_NOTICE.replace("kWriteNoticeKindDrop", "kWriteNoticeKindDropped")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, write_notice=renamed)


@pytest.mark.parametrize(
    "ts_name", ["CONFIG_GET_METHOD", "CONFIG_SET_METHOD", "CONFIG_THEME_KEY"])
def test_config_vocabulary_drift_fails(tmp_path: Path, ts_name: str) -> None:
    """The e06d config surface: a drift here leaves the theme applying and never PERSISTING.

    Worth its own case per member because the symptom is DELAYED — the editor looks right for the whole
    session and the choice is simply gone at the next launch, by which point nothing connects it to a
    renamed constant.
    """
    drifted = re.sub(rf'({ts_name} = ")[^"]*(")', r"\1drifted\2", PANEL_BUNDLE)
    assert drifted != PANEL_BUNDLE
    assert _run_panel(tmp_path, bundle=drifted) == 1


@pytest.mark.parametrize(
    "ts_name", ["CONFIG_GET_METHOD", "CONFIG_SET_METHOD", "CONFIG_THEME_KEY"])
def test_bundle_missing_a_config_constant_fails(tmp_path: Path, ts_name: str) -> None:
    """An ABSENT config constant means editor-core is not on the surface the Shell serves."""
    stripped = "\n".join(line for line in PANEL_BUNDLE.splitlines() if ts_name not in line)
    assert _run_panel(tmp_path, bundle=stripped + "\n") == 1


def test_a_renamed_config_cpp_constant_is_a_config_error(tmp_path: Path) -> None:
    """Rot-into-a-no-op guard for the e06d surface: exit 2, never a silent pass."""
    renamed = PANEL_CPP_CONFIG.replace("kConfigSetMethod", "kConfigStoreMethod")
    with pytest.raises(check_webui_assets.CheckError):
        _run_panel(tmp_path, config=renamed)


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
# regression found while `editor-cef-smoke-shell` was red on ubuntu + windows at M9 e06b: a bare
# `.dockview-theme-dark` block ties dockview's own specificity, loses on document order to the
# RUNTIME-injected copy, and the docking chrome silently keeps the engine's stock greys — with the
# live CEF smoke as the only signal, a full CI round-trip away. (It was a REAL defect but not the
# whole cause of those red legs; the theme-contract cases at the end of this file pin the other one.)


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


# --- the M9 e06b theme-contract gate (--theme-contract) ------------------------------------------
#
# The regression pinned here is the SECOND, deeper cause of the same red legs the specificity gate
# above addresses. `colors.panel` is a PER-THEME value, and editor-core's first run follows the
# host's `prefers-color-scheme` (design 06 §4 / C-F22). A CI host has no colour-scheme preference at
# all — no settings portal — so Chromium falls back to `light`, the editor honestly boots
# `builtin.light` (#ffffff), and a smoke hardcoding the DARK panel colour (#0a0a0a) finds zero
# matching texels on a perfectly healthy frame. It was green on a dark-mode dev box and red on both
# CI legs, and the only signal was a full CI round-trip. Each smoke now PINS the theme it means, and
# this gate keeps the pinned id, the hardcoded bytes and the theme JSON in lockstep.

# The boot-URL statement that actually carries the pin — named so the cases below can remove it
# without re-spelling it, which is how a "declared but never used" fixture stays honest.
PINNED_URL_STATEMENT = (
    'cef_options.url = std::string(shell::kAppEntryUrl) + "?" + shell::kThemePinFlag + "=" +\n'
    "                  kSmokeThemeId;\n"
)

GOOD_THEME_SMOKE = (
    "constexpr std::uint8_t kAppBackgroundB = 0x0a;\n"
    "constexpr std::uint8_t kAppBackgroundG = 0x0a;\n"
    "constexpr std::uint8_t kAppBackgroundR = 0x0a;\n"
    'constexpr const char* kSmokeThemeId = "builtin.dark";\n'
    + PINNED_URL_STATEMENT
)


def _theme_fixture(tmp_path: Path, *, smoke: str = GOOD_THEME_SMOKE,
                   panel: str = "#0a0a0a") -> tuple[Path, Path]:
    cef_dir = tmp_path / "cefsrc"
    cef_dir.mkdir(parents=True, exist_ok=True)
    for name in check_webui_assets.THEME_SMOKES:
        (cef_dir / name).write_text(smoke, encoding="utf-8")
    themes_dir = tmp_path / "themes"
    themes_dir.mkdir(parents=True, exist_ok=True)
    (themes_dir / "dark.theme.json").write_text(
        json.dumps({"colors": {"panel": panel}}), encoding="utf-8")
    return cef_dir, themes_dir


def test_theme_contract_happy_path(tmp_path: Path, capsys) -> None:
    cef_dir, themes_dir = _theme_fixture(tmp_path)
    assert check_webui_assets.run_theme_contract(cef_dir, themes_dir) == 0
    assert "theme contract OK" in capsys.readouterr().out


def test_a_theme_whose_panel_colour_moved_fails(tmp_path: Path) -> None:
    """THE regression: editing the theme's panel colour without moving kAppBackground* must red."""
    cef_dir, themes_dir = _theme_fixture(tmp_path, panel="#ffffff")
    failures = check_webui_assets.check_theme_contract(cef_dir, themes_dir)
    assert len(failures) == len(check_webui_assets.THEME_SMOKES)
    assert "#0a0a0a" in failures[0] and "#ffffff" in failures[0]


def test_a_smoke_that_declares_a_pin_but_never_uses_it_fails(tmp_path: Path) -> None:
    """A declared-but-unused pin leaves the theme on the HOST's preference — the original defect."""
    smoke = GOOD_THEME_SMOKE.replace(
        PINNED_URL_STATEMENT, "cef_options.url = shell::kAppEntryUrl;\n")
    cef_dir, themes_dir = _theme_fixture(tmp_path, smoke=smoke)
    failures = check_webui_assets.check_theme_contract(cef_dir, themes_dir)
    assert len(failures) == len(check_webui_assets.THEME_SMOKES)
    assert "prefers-color-scheme" in failures[0]


def test_a_pin_mentioned_only_in_a_COMMENT_does_not_satisfy_the_gate(tmp_path: Path) -> None:
    """The smokes explain the pin in prose, so a raw substring probe would be unfalsifiable."""
    smoke = GOOD_THEME_SMOKE.replace(
        PINNED_URL_STATEMENT,
        "// the boot URL used to carry shell::kThemePinFlag here\n"
        "cef_options.url = shell::kAppEntryUrl;\n")
    cef_dir, themes_dir = _theme_fixture(tmp_path, smoke=smoke)
    failures = check_webui_assets.check_theme_contract(cef_dir, themes_dir)
    assert len(failures) == len(check_webui_assets.THEME_SMOKES)
    assert "prefers-color-scheme" in failures[0]


def test_a_non_builtin_pinned_theme_is_a_config_error(tmp_path: Path) -> None:
    """A user theme is not present on a CI host, so a smoke may only pin a built-in."""
    smoke = GOOD_THEME_SMOKE.replace('"builtin.dark"', '"user.mine"')
    cef_dir, themes_dir = _theme_fixture(tmp_path, smoke=smoke)
    with pytest.raises(check_webui_assets.CheckError, match="built-in"):
        check_webui_assets.check_theme_contract(cef_dir, themes_dir)


def test_a_non_hex_panel_colour_is_a_config_error(tmp_path: Path) -> None:
    """The smokes compare raw bytes; a named/rgba() colour must fail loudly, never be skipped."""
    cef_dir, themes_dir = _theme_fixture(tmp_path, panel="rgba(10,10,10,1)")
    with pytest.raises(check_webui_assets.CheckError, match="rrggbb"):
        check_webui_assets.check_theme_contract(cef_dir, themes_dir)


def test_a_renamed_smoke_constant_is_a_config_error(tmp_path: Path) -> None:
    smoke = GOOD_THEME_SMOKE.replace("kAppBackgroundR", "kAppBgR")
    cef_dir, themes_dir = _theme_fixture(tmp_path, smoke=smoke)
    with pytest.raises(check_webui_assets.CheckError, match="kAppBackgroundR"):
        check_webui_assets.check_theme_contract(cef_dir, themes_dir)


def test_a_missing_smoke_is_a_config_error(tmp_path: Path) -> None:
    cef_dir, themes_dir = _theme_fixture(tmp_path)
    (cef_dir / check_webui_assets.THEME_SMOKES[0]).unlink()
    with pytest.raises(check_webui_assets.CheckError, match="no longer exists"):
        check_webui_assets.check_theme_contract(cef_dir, themes_dir)


def test_main_routes_the_theme_contract_flag(tmp_path: Path) -> None:
    cef_dir, themes_dir = _theme_fixture(tmp_path)
    assert check_webui_assets.main([
        "--asset-dir", str(tmp_path), "--theme-contract",
        "--shell-cef-dir", str(cef_dir), "--themes-dir", str(themes_dir),
    ]) == 0


def test_the_real_smokes_agree_with_the_real_themes() -> None:
    """Ground truth, not a fixture: every SHIPPED CEF smoke pins a theme that paints its scan colour."""
    cef_dir = REPO_ROOT / "src" / "editor" / "shell" / "cef" / "src"
    themes_dir = REPO_ROOT / "src" / "editor" / "webui" / "tokens" / "themes"
    assert check_webui_assets.check_theme_contract(cef_dir, themes_dir) == []
