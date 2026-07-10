"""Tests for tools/fetch_tint.py — the pinned Tint build/verify gate (R-QA-013).

Covers manifest validation (missing keys, malformed commit), the fail-closed commit
verification (a re-pointed tag is refused before any build), the idempotent stamp no-op
path (a staged+stamped binary needs neither git nor cmake), and the exit-code contract —
all with the subprocess seams monkeypatched so no network / compiler is touched. A final
integration block asserts the REAL tools/tint-toolchain.json pin manifest is well-formed,
keeping the pin itself honest (the test_fetch_esbuild.py / test_fetch_naga.py pattern).
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import load_tool

fetch_tint = load_tool("fetch_tint")

PIN = {
    "repository": "https://example.invalid/dawn",
    "tag": "v20990101.000000",
    "commit": "a" * 40,
    "target": "tint_cmd_tint_cmd",
    "binary": "tint",
    "cmake_args": ["-DCMAKE_BUILD_TYPE=Release"],
}


def _write_manifest(tmp_path: Path, data: dict) -> Path:
    path = tmp_path / "tint-toolchain.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


# --- manifest validation -------------------------------------------------------------


def test_load_manifest_happy(tmp_path: Path) -> None:
    manifest = fetch_tint.load_manifest(_write_manifest(tmp_path, PIN))
    assert manifest["target"] == "tint_cmd_tint_cmd"


@pytest.mark.parametrize("missing", list(fetch_tint.MANIFEST_KEYS))
def test_load_manifest_missing_key_fails(tmp_path: Path, missing: str) -> None:
    data = {k: v for k, v in PIN.items() if k != missing}
    with pytest.raises(fetch_tint.FetchError, match=missing):
        fetch_tint.load_manifest(_write_manifest(tmp_path, data))


def test_load_manifest_short_commit_fails(tmp_path: Path) -> None:
    data = dict(PIN, commit="abc123")
    with pytest.raises(fetch_tint.FetchError, match="40-hex"):
        fetch_tint.load_manifest(_write_manifest(tmp_path, data))


# --- fail-closed commit verification ---------------------------------------------------


def test_clone_and_verify_rejects_commit_mismatch(tmp_path: Path,
                                                  monkeypatch: pytest.MonkeyPatch) -> None:
    """A tag resolving to a different commit than the pin is refused BEFORE any build."""
    monkeypatch.setattr(fetch_tint, "run", lambda *_a, **_k: None)  # the clone itself

    class FakeProc:
        returncode = 0
        stdout = "b" * 40 + "\n"

    monkeypatch.setattr(fetch_tint.subprocess, "run", lambda *_a, **_k: FakeProc())
    with pytest.raises(fetch_tint.VerifyError, match="refusing to build"):
        fetch_tint.clone_and_verify(PIN, tmp_path)


def test_run_missing_tool_is_config_error(monkeypatch: pytest.MonkeyPatch) -> None:
    """A missing git/cmake maps to the documented exit-2 FetchError, not an uncaught traceback."""

    def raise_missing(*_a, **_k):
        raise FileNotFoundError(2, "No such file or directory", "git")

    monkeypatch.setattr(fetch_tint.subprocess, "run", raise_missing)
    with pytest.raises(fetch_tint.FetchError, match="cannot run 'git'"):
        fetch_tint.run(["git", "clone", "https://example.invalid/dawn"])


# --- main() paths ---------------------------------------------------------------------


def test_main_noop_when_stamp_and_smoke_match(tmp_path: Path,
                                              monkeypatch: pytest.MonkeyPatch) -> None:
    """Stamp match + staged binary + passing smoke exits 0 without git/cmake (cache hit)."""
    manifest_path = _write_manifest(tmp_path, PIN)
    binary = fetch_tint.binary_path(tmp_path, PIN)
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7ffake-tint")
    fetch_tint.stamp_path(tmp_path).write_text(
        json.dumps({"tag": PIN["tag"], "commit": PIN["commit"]}), encoding="utf-8")
    monkeypatch.setattr(fetch_tint, "smoke", lambda _b: None)

    def boom(*_a, **_k):
        raise AssertionError("clone/build must not run on the no-op path")

    monkeypatch.setattr(fetch_tint, "clone_and_verify", boom)
    monkeypatch.setattr(fetch_tint.sys, "argv",
                        ["fetch_tint", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_tint.main() == 0


def test_main_stale_stamp_rebuilds(tmp_path: Path,
                                   monkeypatch: pytest.MonkeyPatch) -> None:
    """A stamp from a DIFFERENT pin does not satisfy the no-op path."""
    manifest_path = _write_manifest(tmp_path, PIN)
    binary = fetch_tint.binary_path(tmp_path, PIN)
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"\x7fstale-tint")
    fetch_tint.stamp_path(tmp_path).write_text(
        json.dumps({"tag": "v-old", "commit": "c" * 40}), encoding="utf-8")

    calls = {"rebuilt": False}

    def fake_clone(_m, _w):
        calls["rebuilt"] = True
        raise fetch_tint.FetchError("stop here — rebuild path taken")

    monkeypatch.setattr(fetch_tint, "clone_and_verify", fake_clone)
    monkeypatch.setattr(fetch_tint.sys, "argv",
                        ["fetch_tint", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_tint.main() == 2
    assert calls["rebuilt"]


def test_main_verify_failure_exits_1(tmp_path: Path,
                                     monkeypatch: pytest.MonkeyPatch) -> None:
    manifest_path = _write_manifest(tmp_path, PIN)

    def raise_verify(_m, _w):
        raise fetch_tint.VerifyError("verification FAILED (test)")

    monkeypatch.setattr(fetch_tint, "clone_and_verify", raise_verify)
    monkeypatch.setattr(fetch_tint.sys, "argv",
                        ["fetch_tint", "--manifest", str(manifest_path),
                         "--dest", str(tmp_path)])
    assert fetch_tint.main() == 1


# --- the real pin manifest stays honest ------------------------------------------------


def test_real_manifest_is_well_formed() -> None:
    manifest = fetch_tint.load_manifest(fetch_tint.DEFAULT_MANIFEST)
    assert manifest["binary"] == "tint"
    assert manifest["repository"].startswith("https://")
    assert len(manifest["commit"]) == 40
    assert all(c in "0123456789abcdef" for c in manifest["commit"])
    # The flags the measured evaluation (and the CI build) depend on must stay present.
    for required in ("-DDAWN_FETCH_DEPENDENCIES=ON", "-DTINT_BUILD_CMD_TOOLS=ON",
                     "-DTINT_BUILD_SPV_READER=ON", "-DTINT_BUILD_WGSL_WRITER=ON"):
        assert required in manifest["cmake_args"], required
