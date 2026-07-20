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

# The four constants the bundle must agree with the Shell about.
SCHEME_BUNDLE = (
    'var BRIDGE_SCHEME = "context-editor";\n'
    'var BRIDGE_ORIGIN = "context-editor://app";\n'
    'var BRIDGE_ENDPOINT = "context-editor://ipc";\n'
    'var BRIDGE_QUERY_FUNCTION = "contextEditorQuery";\n'
)

CPP_HEADER = (
    'inline constexpr const char* kAppScheme = "context-editor";\n'
    'inline constexpr const char* kAppOrigin = "context-editor://app";\n'
    'inline constexpr const char* kIpcEndpoint = "context-editor://ipc";\n'
)

CPP_CEF = 'constexpr const char* kBridgeQueryFunction = "contextEditorQuery";\n'


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
])
def test_csp_violating_document_fails(tmp_path: Path, bad_document: str) -> None:
    """Each of these is BLOCKED by the served CSP at runtime, so it must fail at BUILD time."""
    assert _run_scheme(tmp_path, document=bad_document) == 1


def test_missing_stylesheet_fails(tmp_path: Path) -> None:
    assert _run_scheme(tmp_path, stylesheet=None) == 1


def test_missing_document_fails(tmp_path: Path) -> None:
    asset_dir, include_dir, cef_dir = _scheme_fixture(tmp_path)
    (asset_dir / "index.html").unlink()
    assert check_webui_assets.run_scheme_contract(
        asset_dir, "editor-core.js", include_dir, cef_dir) == 1


@pytest.mark.parametrize("ts_name,drifted", [
    ("BRIDGE_SCHEME", "context-edit"),
    ("BRIDGE_ORIGIN", "context-editor://application"),
    ("BRIDGE_ENDPOINT", "context-editor://bridge"),
    ("BRIDGE_QUERY_FUNCTION", "cefQuery"),
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
    _human, cef_file, cpp_name, ts_name = check_webui_assets.QUERY_FUNCTION_CONSTANT
    cpp_value = check_webui_assets._read_cpp_string_constant(cef / cef_file, cpp_name)
    assert f'{ts_name} = "{cpp_value}"' in ts
