"""Tests for tools/fetch_cef.py — the pinned CEF prebuilt fetch/verify gate (R-QA-013).

Covers the happy path (offline ``--source``, exercising the real verify + bz2 tar extraction with
prefix-strip + symlink/mode preservation), every fail-closed path (SHA mismatch, unknown triple,
filename↔version skew, missing source file, path-traversal guard), and idempotency — all on tiny
synthetic fixtures so no network is touched. A final integration block asserts the REAL
tools/cef-prebuilt.json is well-formed, coherent, and complete for the 3 desktop triples, keeping the
pin manifest itself honest.
"""

from __future__ import annotations

import io
import json
import tarfile
from pathlib import Path

import pytest
from conftest import load_tool

fetch_cef = load_tool("fetch_cef")

VERSION = "9.9.9+gabc+chromium-9.9.9.9"
PREFIX = f"cef_binary_{VERSION}_linux64_minimal"
ARCHIVE_NAME = f"{PREFIX}.tar.bz2"
# Files inside the synthetic distribution (mirrors the shape fetch_cef strips + stages).
DISTRO_FILES = {
    "include/cef_version.h": b"// fake cef_version.h\n",
    "cmake/FindCEF.cmake": b"# fake FindCEF\n",
    "libcef_dll/base/cef_ref_counted.cc": b"// fake wrapper source\n",
    "Release/libcef.so": b"\x7fELF fake libcef\n",
    "CMakeLists.txt": b"# fake top-level CMakeLists\n",
}


def _make_archive_bytes() -> bytes:
    """Build a .tar.bz2 with a single top-level PREFIX dir, a nested file tree, and a symlink."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:bz2") as tar:
        # top-level dir entry
        d = tarfile.TarInfo(name=PREFIX + "/")
        d.type = tarfile.DIRTYPE
        d.mode = 0o755
        tar.addfile(d)
        for rel, data in DISTRO_FILES.items():
            info = tarfile.TarInfo(name=f"{PREFIX}/{rel}")
            info.size = len(data)
            info.mode = 0o755 if rel.endswith(".so") else 0o644
            tar.addfile(info, io.BytesIO(data))
        # a relative symlink (the macOS framework relies on symlink preservation)
        link = tarfile.TarInfo(name=f"{PREFIX}/Release/libcef.so.1")
        link.type = tarfile.SYMTYPE
        link.linkname = "libcef.so"
        tar.addfile(link)
    return buf.getvalue()


def _make_source(tmp_path: Path) -> tuple[Path, dict]:
    """Build a synthetic --source dir (the .tar.bz2) and a matching manifest."""
    source = tmp_path / "source"
    source.mkdir()
    archive = _make_archive_bytes()
    (source / ARCHIVE_NAME).write_bytes(archive)
    manifest = {
        "cef_version": VERSION,
        "chromium_version": "9.9.9.9",
        "distribution": "minimal",
        "url_template": "https://example.invalid/{url_file}",
        "archives": {
            "x86_64-unknown-linux-gnu": {
                "platform": "linux64",
                "file": ARCHIVE_NAME,
                "sha256": fetch_cef.hashlib.sha256(archive).hexdigest(),
                "sha1": "0" * 40,
                "size": len(archive),
            },
        },
    }
    return source, manifest


def _write_manifest(tmp_path: Path, manifest: dict) -> Path:
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def test_offline_happy_path_stages_distribution(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    result = fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)

    assert result["cached"] is False
    root = Path(result["root"])
    for rel, data in DISTRO_FILES.items():
        assert (root / rel).read_bytes() == data  # prefix stripped, nested dirs kept
    # symlink preserved (not dereferenced into a copy)
    assert (root / "Release" / "libcef.so.1").is_symlink()


def test_second_run_is_cached(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    dest = tmp_path / "out"

    fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)
    second = fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", dest, source=source)
    assert second["cached"] is True


def test_sha_mismatch_is_refused_fail_closed(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["archives"]["x86_64-unknown-linux-gnu"]["sha256"] = "0" * 64  # tampered pin
    manifest_path = _write_manifest(tmp_path, manifest)

    with pytest.raises(fetch_cef.VerifyError):
        fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_sha_mismatch_exit_code_is_one(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["archives"]["x86_64-unknown-linux-gnu"]["sha256"] = "0" * 64
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_cef.main([
        "--manifest", str(manifest_path), "--triple", "x86_64-unknown-linux-gnu",
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 1  # fail-closed refusal


def test_unknown_triple_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_cef.FetchError):
        fetch_cef.fetch(manifest_path, "sparc-unknown-solaris", tmp_path / "out", source=source)


def test_unknown_triple_exit_code_is_two(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest_path = _write_manifest(tmp_path, manifest)
    rc = fetch_cef.main([
        "--manifest", str(manifest_path), "--triple", "sparc-unknown-solaris",
        "--dest", str(tmp_path / "out"), "--source", str(source),
    ])
    assert rc == 2  # configuration/usage error


def test_filename_version_skew_is_rejected(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    manifest["cef_version"] = "1.2.3+other"  # no longer present in the archive file name
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_cef.FetchError, match="version-coherence"):
        fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_missing_source_file_is_config_error(tmp_path: Path):
    source, manifest = _make_source(tmp_path)
    (source / ARCHIVE_NAME).unlink()  # archive absent from --source dir
    manifest_path = _write_manifest(tmp_path, manifest)
    with pytest.raises(fetch_cef.FetchError):
        fetch_cef.fetch(manifest_path, "x86_64-unknown-linux-gnu", tmp_path / "out", source=source)


def test_url_escapes_plus_in_filename():
    manifest = {"url_template": "https://cef-builds.spotifycdn.com/{url_file}"}
    archive = {"file": "cef_binary_149.0.6+g0d0eeb6+chromium-149.0.7827.201_linux64_minimal.tar.bz2"}
    url = fetch_cef._url_for(manifest, archive)
    assert "%2B" in url and "+" not in url.split("/")[-1]


def test_path_traversal_member_is_skipped(tmp_path: Path):
    """A member escaping the prefix (absolute or via '..') is skipped, not written outside dest."""
    assert fetch_cef._safe_relname(f"{PREFIX}/../evil.txt", PREFIX) is None
    assert fetch_cef._safe_relname("/etc/passwd", PREFIX) is None
    assert fetch_cef._safe_relname(f"{PREFIX}/ok/file.txt", PREFIX) == "ok/file.txt"
    assert fetch_cef._safe_relname(PREFIX + "/", PREFIX) is None  # the prefix dir itself


# --- integration: the REAL pin manifest must stay well-formed + coherent + complete ---------

REPO_ROOT = Path(__file__).resolve().parents[2]
REAL_MANIFEST = REPO_ROOT / "tools" / "cef-prebuilt.json"
REQUIRED_TRIPLES = {
    "x86_64-unknown-linux-gnu",
    "aarch64-apple-darwin",
    "x86_64-pc-windows-msvc",
}


def test_real_manifest_is_coherent_and_complete():
    manifest = json.loads(REAL_MANIFEST.read_text(encoding="utf-8"))
    version = manifest["cef_version"]
    archives = manifest["archives"]
    # All three desktop CI matrix triples present + pinned (same triples as tools/v8-prebuilt.json).
    assert REQUIRED_TRIPLES.issubset(archives.keys())
    for triple, arc in archives.items():
        # resolve() enforces filename↔version coherence + required keys.
        v, resolved = fetch_cef.resolve(manifest, triple)
        assert v == version and resolved is arc
        assert len(arc["sha256"]) == 64, triple
        assert all(c in "0123456789abcdefABCDEF" for c in arc["sha256"]), triple  # hex
        assert isinstance(arc["size"], int) and arc["size"] > 0, triple
        assert arc["file"].endswith(".tar.bz2"), triple
        assert arc["platform"] in arc["file"], triple
    # The URL template must accept the documented substitution key without KeyError and escape '+'.
    url = fetch_cef._url_for(manifest, archives["x86_64-unknown-linux-gnu"])
    assert url.startswith("https://") and "%2B" in url


# --- retry/backoff for transient upstream failures (Context-Engine#129) ----------------------


class _FlakyUrlopen:
    """A urlopen stand-in: raises URLError the first `fail_times` calls, then serves bytes."""

    def __init__(self, fail_times: int, payload: bytes = b"payload-bytes"):
        self.fail_times = fail_times
        self.payload = payload
        self.calls = 0

    def __call__(self, url, timeout=120):  # noqa: ARG002
        self.calls += 1
        if self.calls <= self.fail_times:
            raise fetch_cef.urllib.error.URLError("simulated transient 504")
        return io.BytesIO(self.payload)


def test_download_retries_then_succeeds(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=2)
    monkeypatch.setattr(fetch_cef.urllib.request, "urlopen", opener)
    dest = tmp_path / "out.bin"
    slept: list[float] = []
    fetch_cef._download_with_retry(
        "https://example.invalid/x", dest, attempts=4, base_delay=0, sleep=slept.append)
    assert dest.read_bytes() == b"payload-bytes"
    assert opener.calls == 3   # two transient failures + one success
    assert len(slept) == 2     # backed off before each retry


def test_download_gives_up_after_attempts(tmp_path: Path, monkeypatch):
    opener = _FlakyUrlopen(fail_times=99)
    monkeypatch.setattr(fetch_cef.urllib.request, "urlopen", opener)
    with pytest.raises(fetch_cef.FetchError, match="after 3 attempts"):
        fetch_cef._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=3, base_delay=0,
            sleep=lambda *_: None)
    assert opener.calls == 3


def test_download_backoff_grows_exponentially(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(fetch_cef.urllib.request, "urlopen", _FlakyUrlopen(fail_times=99))
    slept: list[float] = []
    with pytest.raises(fetch_cef.FetchError):
        fetch_cef._download_with_retry(
            "https://example.invalid/x", tmp_path / "o", attempts=4, base_delay=1,
            sleep=slept.append)
    assert slept == [1, 3, 9]  # exponential: base * 3**(attempt-1), no sleep after the last try
