"""Tests for tools/fetch_esbuild.py — the pinned esbuild prebuilt fetch/verify gate (R-QA-013).

Covers the happy path (offline ``--source``, exercising the real verify + tar member
extraction + the executable bit), every fail-closed path (SHA mismatch, unknown platform,
version-coherence skew, missing source file, missing binary member), and idempotency — all
on tiny synthetic npm-style tarballs so no network is touched. A final integration block
asserts the REAL tools/ts-toolchain.json is well-formed, version-coherent, and covers the
three CI platforms, keeping the pin manifest itself honest.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import stat
import tarfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_esbuild = load_tool("fetch_esbuild")

VERSION = "9.9.9"
BINARY_BYTES = b"\x7ffake-esbuild-native-binary\x00\x01\x02"


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _make_package_tgz(member: str) -> bytes:
    """Build a synthetic @esbuild/<platform> npm tarball carrying one binary member."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        info = tarfile.TarInfo(name=member)
        info.size = len(BINARY_BYTES)
        info.mode = 0o755
        tar.addfile(info, io.BytesIO(BINARY_BYTES))
        # A non-binary sibling member proves extraction picks out exactly the pinned member.
        readme = b"# esbuild\n"
        rinfo = tarfile.TarInfo(name="package/README.md")
        rinfo.size = len(readme)
        tar.addfile(rinfo, io.BytesIO(readme))
    return buf.getvalue()


def _make_source(tmp_path: Path, platform: str, member: str, extracted: str) -> tuple[Path, dict]:
    """Build a synthetic --source dir (one npm tarball) and a matching manifest."""
    source = tmp_path / "source"
    source.mkdir()
    tgz = _make_package_tgz(member)
    file_name = f"{platform}-{VERSION}.tgz"
    (source / file_name).write_bytes(tgz)

    manifest = {
        "esbuild_version": VERSION,
        "package_url_template": "https://example.invalid/@esbuild/{platform}/{esbuild_version}",
        "platforms": {
            platform: {
                "package": f"@esbuild/{platform}",
                "file": file_name,
                "sha256": _sha(tgz),
                "member": member,
                "extracted": extracted,
            },
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_offline_happy_path_stages_binary(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    result = fetch_esbuild.fetch(manifest_path, "linux-x64", dest, source=source)

    assert result["cached"] is False
    binary = Path(result["binary"])
    assert binary.name == "esbuild"
    assert binary.read_bytes() == BINARY_BYTES  # exactly the pinned member, byte-exact
    if os.name != "nt":
        assert binary.stat().st_mode & stat.S_IXUSR  # made executable on POSIX


def test_win32_member_and_extracted_name(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "win32-x64", "package/esbuild.exe", "esbuild.exe")
    manifest_path = _write_manifest(tmp_path, manifest)
    result = fetch_esbuild.fetch(manifest_path, "win32-x64", tmp_path / "out", source=source)
    assert Path(result["binary"]).name == "esbuild.exe"
    assert Path(result["binary"]).read_bytes() == BINARY_BYTES


def test_second_run_is_cached(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    fetch_esbuild.fetch(manifest_path, "linux-x64", dest, source=source)
    second = fetch_esbuild.fetch(manifest_path, "linux-x64", dest, source=source)
    assert second["cached"] is True


def test_sha_mismatch_is_refused_fail_closed(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest["platforms"]["linux-x64"]["sha256"] = "0" * 64  # tampered pin
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_esbuild.VerifyError):
        fetch_esbuild.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_sha_mismatch_exit_code_is_one(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest["platforms"]["linux-x64"]["sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_esbuild.main([
        "--manifest", str(manifest_path), "--platform", "linux-x64",
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 1  # fail-closed refusal


def test_unknown_platform_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_esbuild.FetchError):
        fetch_esbuild.fetch(manifest_path, "sparc-solaris", tmp_path / "out", source=source)


def test_version_coherence_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest["platforms"]["linux-x64"]["file"] = "linux-x64-1.2.3.tgz"  # file version != esbuild_version
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_esbuild.FetchError, match="version-coherence"):
        fetch_esbuild.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_package_key_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest["platforms"]["linux-x64"]["package"] = "@esbuild/darwin-arm64"  # mismatched key
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_esbuild.FetchError, match="version-coherence"):
        fetch_esbuild.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_missing_source_file_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    (source / manifest["platforms"]["linux-x64"]["file"]).unlink()  # tarball absent from --source
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_esbuild.FetchError):
        fetch_esbuild.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


def test_missing_binary_member_is_config_error(tmp_path: Path):
    # Manifest names a member the tarball does not carry — a layout-change tripwire.
    source, manifest = _make_source(tmp_path, "linux-x64", "package/bin/esbuild", "esbuild")
    manifest["platforms"]["linux-x64"]["member"] = "package/bin/nonexistent"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_esbuild.FetchError, match="member"):
        fetch_esbuild.fetch(manifest_path, "linux-x64", tmp_path / "out", source=source)


# --- integration: the REAL pin manifest must stay well-formed + coherent -------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
REAL_MANIFEST = REPO_ROOT / "tools" / "ts-toolchain.json"
REQUIRED_PLATFORMS = {"linux-x64", "darwin-arm64", "win32-x64"}


def test_real_manifest_is_coherent_and_complete():
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    version = fetch_esbuild.check_coherence(manifest)  # raises on skew
    assert version == manifest["esbuild_version"]
    # All three CI matrix platforms (Linux-x64 / macOS-ARM64 / Win-x64) present + pinned.
    assert REQUIRED_PLATFORMS.issubset(manifest["platforms"].keys())
    for name, spec in manifest["platforms"].items():
        assert len(spec["sha256"]) == 64, name
        assert all(c in "0123456789abcdefABCDEF" for c in spec["sha256"]), name  # hex
        assert spec["file"] and spec["member"] and spec["extracted"], name
    # win32 ships esbuild.exe; the unix platforms ship a bare `esbuild`.
    assert manifest["platforms"]["win32-x64"]["extracted"] == "esbuild.exe"
    assert manifest["platforms"]["linux-x64"]["extracted"] == "esbuild"
    # The URL template must accept the documented substitution keys without KeyError.
    formatted = manifest["package_url_template"].format(
        platform="linux-x64", esbuild_version=version)
    assert formatted.startswith("https://")
