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
