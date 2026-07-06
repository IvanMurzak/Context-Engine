"""Tests for tools/fetch_v8.py — the pinned V8 prebuilt fetch/verify gate (R-QA-013).

Covers the happy path (offline ``--source``, exercising the real verify + gunzip + crate
extraction), every fail-closed path (SHA mismatch, unknown triple, version-coherence skew,
missing source file), and idempotency — all on tiny synthetic fixtures so no network is
touched. A final integration block asserts the REAL tools/v8-prebuilt.json is well-formed
and version-coherent, keeping the pin manifest itself honest.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import tarfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_v8 = load_tool("fetch_v8")

VERSION = "9.9.9"
LIB_BYTES = b"fake-static-archive-contents\x00\x01\x02"
HEADER_FILES = {
    "v8-isolate.h": b"// fake v8-isolate.h\n",
    "v8-inspector.h": b"// fake v8-inspector.h\n",
    "cppgc/heap.h": b"// fake cppgc/heap.h\n",
}


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _make_source(tmp_path: Path) -> tuple[Path, dict]:
    """Build a synthetic --source dir (gzipped lib + crate tar.gz) and a matching manifest."""
    source = tmp_path / "source"
    source.mkdir()

    lib_gz = gzip.compress(LIB_BYTES)
    lib_name = "librusty_v8_release_x86_64-unknown-linux-gnu.a.gz"
    (source / lib_name).write_bytes(lib_gz)

    crate_name = f"v8-{VERSION}.crate"
    prefix = f"v8-{VERSION}/v8/include"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for rel, data in HEADER_FILES.items():
            info = tarfile.TarInfo(name=f"{prefix}/{rel}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    crate_bytes = buf.getvalue()
    (source / crate_name).write_bytes(crate_bytes)

    manifest = {
        "rusty_v8_version": VERSION,
        "v8_version": "9.9.9.9",
        "lib_release_url_template": "https://example.invalid/v{rusty_v8_version}/{file}",
        "libs": {
            "x86_64-unknown-linux-gnu": {
                "file": lib_name,
                "sha256": _sha(lib_gz),
                "extracted": "librusty_v8.a",
            },
        },
        "headers": {
            "crate": crate_name,
            "url": "https://example.invalid/crate",
            "sha256": _sha(crate_bytes),
            "include_prefix": prefix,
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_offline_happy_path_stages_lib_and_headers(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    result = fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)

    assert result["cached"] is False
    lib = Path(result["lib"])
    assert lib.name == "librusty_v8.a"
    assert lib.read_bytes() == LIB_BYTES  # gunzipped byte-exact
    include = Path(result["include"])
    for rel, data in HEADER_FILES.items():
        assert (include / rel).read_bytes() == data  # prefix stripped, nested dirs kept


def test_second_run_is_cached(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)
    second = fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)
    assert second["cached"] is True


def test_sha_mismatch_is_refused_fail_closed(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["libs"]["x86_64-unknown-linux-gnu"]["sha256"] = "0" * 64  # tampered pin
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_v8.VerifyError):
        fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_sha_mismatch_exit_code_is_one(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["headers"]["sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_v8.main([
        "--manifest", str(manifest_path), "--triple", "x86_64-unknown-linux-gnu",
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 1  # fail-closed refusal


def test_unknown_triple_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_v8.FetchError):
        fetch_v8.fetch(manifest_path, "sparc-unknown-solaris", tmp_path / "out", source=source)


def test_version_coherence_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["headers"]["crate"] = "v8-1.2.3.crate"  # header version != rusty_v8_version
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_v8.FetchError, match="version-coherence"):
        fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_include_prefix_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["headers"]["include_prefix"] = "v8-1.2.3/v8/include"
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_v8.FetchError, match="version-coherence"):
        fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_missing_source_file_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    (source / manifest["headers"]["crate"]).unlink()  # crate absent from --source dir
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_v8.FetchError):
        fetch_v8.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


# --- integration: the REAL pin manifest must stay well-formed + coherent -------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
REAL_MANIFEST = REPO_ROOT / "tools" / "v8-prebuilt.json"
REQUIRED_TRIPLES = {
    "x86_64-unknown-linux-gnu",
    "aarch64-apple-darwin",
    "x86_64-pc-windows-msvc",
}


def test_real_manifest_is_coherent_and_complete():
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    version = fetch_v8.check_coherence(manifest)  # raises on skew
    assert version == manifest["rusty_v8_version"]
    # All three CI matrix triples (Linux-x64 / macOS-ARM64 / Win-x64) present + pinned.
    assert REQUIRED_TRIPLES.issubset(manifest["libs"].keys())
    for triple, lib in manifest["libs"].items():
        assert len(lib["sha256"]) == 64, triple
        assert all(c in "0123456789abcdefABCDEF" for c in lib["sha256"]), triple  # hex
        assert lib["file"] and lib["extracted"], triple
    assert len(manifest["headers"]["sha256"]) == 64
    # The URL template must accept the documented substitution keys without KeyError.
    formatted = manifest["lib_release_url_template"].format(
        rusty_v8_version=version, file=manifest["libs"]["x86_64-unknown-linux-gnu"]["file"])
    assert formatted.startswith("https://")
