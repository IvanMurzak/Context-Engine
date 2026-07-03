"""Tests for tools/check_toolchain.py — the L-42 pinned-toolchain manifest gate (R-QA-013).

Unit tests exercise version extraction/matching and every enforcement level against a
throwaway manifest under tmp_path; the integration tests at the bottom run against the
repo's live cmake/toolchain-versions.json to keep the manifest itself honest.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOLS_DIR.parent

_spec = importlib.util.spec_from_file_location("check_toolchain", TOOLS_DIR / "check_toolchain.py")
check_toolchain = importlib.util.module_from_spec(_spec)
sys.modules["check_toolchain"] = check_toolchain
_spec.loader.exec_module(check_toolchain)


def make_manifest(tmp_path: Path, targets: dict) -> Path:
    path = tmp_path / "toolchain-versions.json"
    path.write_text(json.dumps({"manifest_version": 1, "targets": targets}), encoding="utf-8")
    return path


STRICT_CLANG = {
    "compiler": "clang",
    "l42_target": "mainline clang, pinned",
    "pin": "20.1",
    "actual": "clang 20.1 (apt.llvm.org)",
    "enforcement": "strict",
    "install": {"method": "apt.llvm.org (llvm.sh)", "major": 20},
}

ADVISORY_APPLE = {
    "compiler": "apple-clang",
    "l42_target": "Apple clang via the macOS build agent",
    "pin": "17.0",
    "actual": "runner-default Apple clang, recorded",
    "enforcement": "advisory",
}

DOCUMENTED_MSVC = {
    "compiler": "msvc",
    "l42_target": "clang-cl + MSVC STL",
    "pin": None,
    "actual": "runner-default MSVC",
    "enforcement": "documented",
}


# ---------------------------------------------------------------- version extraction


@pytest.mark.parametrize(
    ("output", "expected"),
    [
        ("Ubuntu clang version 20.1.8 (++20250804090239+87f0227cb601)", "20.1.8"),
        ("Apple clang version 17.0.0 (clang-1700.0.13.3)", "17.0.0"),
        ("Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64", "19.44.35211"),
        ("clang version 21.1.8", "21.1.8"),
        ("no digits here at all", None),
        ("major-only 20 is not a dotted version", None),
    ],
)
def test_extract_version(output, expected):
    assert check_toolchain.extract_version(output) == expected


@pytest.mark.parametrize(
    ("actual", "pin", "expected"),
    [
        ("20.1.8", "20.1", True),
        ("20.1", "20.1", True),
        ("20.10.1", "20.1", False),  # component-wise, not string-prefix
        ("21.1.8", "20.1", False),
        ("17.0.0", "17.0", True),
        ("20", "20.1", False),  # fewer components than the pin never matches
    ],
)
def test_version_matches(actual, pin, expected):
    assert check_toolchain.version_matches(actual, pin) is expected


# ---------------------------------------------------------------- enforcement levels


def test_strict_match_passes(tmp_path):
    manifest = make_manifest(tmp_path, {"linux-x86_64": STRICT_CLANG})
    rc = check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "linux-x86_64",
         "--verify", "Ubuntu clang version 20.1.8"]
    )
    assert rc == 0


def test_strict_mismatch_fails(tmp_path, capsys):
    manifest = make_manifest(tmp_path, {"linux-x86_64": STRICT_CLANG})
    rc = check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "linux-x86_64",
         "--verify", "Ubuntu clang version 21.1.8"]
    )
    assert rc == 1
    assert "pins 20.1" in capsys.readouterr().err


def test_advisory_mismatch_warns_but_passes(tmp_path, capsys):
    manifest = make_manifest(tmp_path, {"macos-arm64": ADVISORY_APPLE})
    rc = check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "macos-arm64",
         "--verify", "Apple clang version 16.0.0 (clang-1600.0.26.6)"]
    )
    assert rc == 0
    assert "::warning" in capsys.readouterr().out


def test_advisory_match_is_quiet_ok(tmp_path, capsys):
    manifest = make_manifest(tmp_path, {"macos-arm64": ADVISORY_APPLE})
    rc = check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "macos-arm64",
         "--verify", "Apple clang version 17.0.0 (clang-1700.0.13.3)"]
    )
    assert rc == 0
    assert "::warning" not in capsys.readouterr().out


def test_documented_entry_rejects_verify(tmp_path):
    manifest = make_manifest(tmp_path, {"windows-x86_64": DOCUMENTED_MSVC})
    with pytest.raises(SystemExit) as excinfo:
        check_toolchain.main(
            ["--manifest", str(manifest), "--platform", "windows-x86_64",
             "--verify", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211"]
        )
    assert excinfo.value.code == 2


def test_unparseable_compiler_output_is_config_error(tmp_path):
    manifest = make_manifest(tmp_path, {"linux-x86_64": STRICT_CLANG})
    with pytest.raises(SystemExit) as excinfo:
        check_toolchain.main(
            ["--manifest", str(manifest), "--platform", "linux-x86_64",
             "--verify", "command not found"]
        )
    assert excinfo.value.code == 2


# ---------------------------------------------------------------- query modes


def test_print_pin_and_install_major(tmp_path, capsys):
    manifest = make_manifest(tmp_path, {"linux-x86_64": STRICT_CLANG})
    assert check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "linux-x86_64", "--print-pin"]) == 0
    assert capsys.readouterr().out.strip() == "20.1"
    assert check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "linux-x86_64", "--print-install-major"]) == 0
    assert capsys.readouterr().out.strip() == "20"


def test_print_pin_without_pin_is_config_error(tmp_path):
    manifest = make_manifest(tmp_path, {"windows-x86_64": DOCUMENTED_MSVC})
    with pytest.raises(SystemExit) as excinfo:
        check_toolchain.main(
            ["--manifest", str(manifest), "--platform", "windows-x86_64", "--print-pin"])
    assert excinfo.value.code == 2


def test_unknown_platform_is_config_error(tmp_path):
    manifest = make_manifest(tmp_path, {"linux-x86_64": STRICT_CLANG})
    with pytest.raises(SystemExit) as excinfo:
        check_toolchain.main(
            ["--manifest", str(manifest), "--platform", "beos-ppc", "--describe"])
    assert excinfo.value.code == 2


def test_describe_prints_target_vs_actual(tmp_path, capsys):
    manifest = make_manifest(tmp_path, {"windows-x86_64": DOCUMENTED_MSVC})
    rc = check_toolchain.main(
        ["--manifest", str(manifest), "--platform", "windows-x86_64", "--describe"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "L-42 target: clang-cl + MSVC STL" in out
    assert "actual:" in out


# ---------------------------------------------------------------- live manifest integration


def test_live_manifest_is_wellformed():
    manifest = check_toolchain.load_manifest(check_toolchain.DEFAULT_MANIFEST)
    targets = manifest["targets"]
    # The three CI platforms + the web leg must all be declared.
    for key in ("linux-x86_64", "macos-arm64", "windows-x86_64", "web-emscripten"):
        assert key in targets, f"manifest is missing '{key}'"
    for key, entry in targets.items():
        assert entry.get("enforcement") in check_toolchain.VALID_ENFORCEMENTS, key
        assert entry.get("l42_target"), f"'{key}' must state its L-42 target"
        assert entry.get("actual"), f"'{key}' must state its actual (target-vs-actual honesty)"


def test_live_manifest_linux_is_strict_and_installable():
    manifest = check_toolchain.load_manifest(check_toolchain.DEFAULT_MANIFEST)
    linux = manifest["targets"]["linux-x86_64"]
    assert linux["enforcement"] == "strict"
    pin = linux["pin"]
    major = linux["install"]["major"]
    # The install major must be the major of the pin, or CI installs one thing and
    # verifies another.
    assert pin.split(".")[0] == str(major)
