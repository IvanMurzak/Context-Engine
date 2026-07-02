"""Tests for tools/check_licenses.py — the deny-by-default dependency-license gate (L-57/O-7).

Retroactive R-QA-013 coverage. Every test but the final integration test builds its own
throwaway repo layout under tmp_path (allowlist + src/vcpkg.json + optional package.json)
and points the gate's module-level paths at it — the repo's live manifests are exercised
only by the single integration-style test at the bottom.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

TOOLS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOLS_DIR.parent

_spec = importlib.util.spec_from_file_location("check_licenses", TOOLS_DIR / "check_licenses.py")
check_licenses = importlib.util.module_from_spec(_spec)
sys.modules["check_licenses"] = check_licenses
_spec.loader.exec_module(check_licenses)


DEFAULT_ALLOWED = ["MIT", "BSD-3-Clause", "Apache-2.0", "Zlib"]


def make_repo(tmp_path: Path,
              allowed=None,
              dependency_licenses=None,
              vcpkg_manifest=...,
              allowlist_text: str | None = None,
              vcpkg_text: str | None = None) -> Path:
    """Build a synthetic repo layout the gate can scan.

    vcpkg_manifest: dict to serialize, None to omit the file entirely, or Ellipsis
    for a minimal empty-dependency manifest. *_text arguments write raw (possibly
    malformed) bytes instead.
    """
    repo = tmp_path / "repo"
    (repo / "tools").mkdir(parents=True)
    (repo / "src").mkdir()

    if allowlist_text is not None:
        (repo / "tools" / "license-allowlist.json").write_text(allowlist_text, encoding="utf-8")
    else:
        allowlist = {
            "allowed_licenses": DEFAULT_ALLOWED if allowed is None else allowed,
            "dependency_licenses": dependency_licenses or {},
        }
        (repo / "tools" / "license-allowlist.json").write_text(
            json.dumps(allowlist, indent=2), encoding="utf-8")

    if vcpkg_text is not None:
        (repo / "src" / "vcpkg.json").write_text(vcpkg_text, encoding="utf-8")
    elif vcpkg_manifest is None:
        pass  # deliberately no manifest
    else:
        manifest = ({"name": "test-project", "version": "1.2.3", "dependencies": []}
                    if vcpkg_manifest is ... else vcpkg_manifest)
        (repo / "src" / "vcpkg.json").write_text(json.dumps(manifest, indent=2),
                                                 encoding="utf-8")
    return repo


def run_gate(repo: Path, monkeypatch) -> tuple[int, dict | None]:
    """Point the gate at `repo`, run it, return (exit_code, sbom_or_None)."""
    monkeypatch.setattr(check_licenses, "REPO_ROOT", repo)
    monkeypatch.setattr(check_licenses, "ALLOWLIST_PATH", repo / "tools" / "license-allowlist.json")
    sbom_path = repo / "sbom.json"
    monkeypatch.setattr(check_licenses, "SBOM_PATH", sbom_path)
    try:
        code = check_licenses.main()
    except SystemExit as exc:  # load_json exits directly on unreadable inputs
        code = exc.code
    sbom = json.loads(sbom_path.read_text(encoding="utf-8")) if sbom_path.is_file() else None
    return code, sbom


def vcpkg(deps=None, features=None, name="test-project", version="1.2.3") -> dict:
    manifest: dict = {"name": name, "version": version, "dependencies": deps or []}
    if features:
        manifest["features"] = features
    return manifest


# ---------------------------------------------------------------------------
# Allow / deny paths
# ---------------------------------------------------------------------------


def test_allowed_license_passes(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path,
                     dependency_licenses={"zlib": "Zlib", "fmt": "MIT"},
                     vcpkg_manifest=vcpkg(deps=["zlib", "fmt"]))
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 0
    assert "PASS" in capsys.readouterr().out
    assert {c["name"] for c in sbom["components"]} == {"zlib", "fmt"}


def test_unlisted_license_fails(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path,
                     dependency_licenses={"weird-lib": "WTFPL"},
                     vcpkg_manifest=vcpkg(deps=["weird-lib"]))
    code, _ = run_gate(repo, monkeypatch)
    assert code == 1
    out = capsys.readouterr().out
    assert "FAIL" in out
    assert "WTFPL" in out and "not on the allowlist" in out


def test_dependency_missing_from_map_fails_deny_by_default(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path,
                     dependency_licenses={},  # nothing recorded
                     vcpkg_manifest=vcpkg(deps=["mystery-lib"]))
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 1
    out = capsys.readouterr().out
    assert "UNKNOWN" in out and "deny-by-default" in out
    # The SBOM still records the component, honestly marked UNKNOWN.
    assert sbom["components"][0]["license"] == "UNKNOWN"


def test_copyleft_in_shipped_core_dependency_fails(tmp_path, monkeypatch, capsys):
    """L-57: copyleft linked into the shipped cores fails — GPL is not allowlistable."""
    repo = make_repo(tmp_path,
                     dependency_licenses={"readline": "GPL-3.0-only"},
                     vcpkg_manifest=vcpkg(deps=["readline"]))
    code, _ = run_gate(repo, monkeypatch)
    assert code == 1
    assert "GPL-3.0-only" in capsys.readouterr().out


def test_no_silent_build_only_allowance(tmp_path, monkeypatch, capsys):
    """Documented behavior: there is NO license bypass for build-only/spike-only deps.

    Feature-scoped dependencies (e.g. the `spikes` feature — never in a shipped core)
    are still scanned and still gated against the single allowlist; the only sanctioned
    way to clear a build-only tool is to record its (allowlisted) license, exactly like
    the v8 spike monolith in the live allowlist.
    """
    features = {"spikes": {"description": "build-only", "dependencies": ["gpl-tool", "nice-tool"]}}
    repo = make_repo(tmp_path,
                     dependency_licenses={"gpl-tool": "GPL-3.0-only", "nice-tool": "MIT"},
                     vcpkg_manifest=vcpkg(features=features))
    code, _ = run_gate(repo, monkeypatch)
    assert code == 1  # copyleft build-only tool fails despite being feature-scoped
    out = capsys.readouterr().out
    assert "gpl-tool" in out
    assert "nice-tool" not in out  # allowlisted build-only tool passes (v8 pattern)


def test_feature_dependencies_are_scanned(tmp_path, monkeypatch):
    """Deps declared only under `features.*.dependencies` (the spikes feature) are scanned."""
    features = {"spikes": {"description": "opt-in", "dependencies": [
        "blake3", {"name": "simdjson", "version>=": "3.10.0"}]}}
    repo = make_repo(tmp_path,
                     dependency_licenses={"blake3": "Apache-2.0", "simdjson": "Apache-2.0"},
                     vcpkg_manifest=vcpkg(features=features))
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 0
    by_name = {c["name"]: c for c in sbom["components"]}
    assert set(by_name) == {"blake3", "simdjson"}
    # Object-form dependency carries its minimum version into the SBOM.
    assert by_name["simdjson"]["version"] == "3.10.0"


def test_mixed_violations_are_all_reported(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path,
                     dependency_licenses={"gpl-lib": "GPL-2.0-only", "ok-lib": "MIT"},
                     vcpkg_manifest=vcpkg(deps=["gpl-lib", "ok-lib", "unmapped-lib"]))
    code, _ = run_gate(repo, monkeypatch)
    assert code == 1
    out = capsys.readouterr().out
    assert "2 violation(s)" in out


# ---------------------------------------------------------------------------
# npm (package.json) scanning
# ---------------------------------------------------------------------------


def test_package_json_dependencies_scanned(tmp_path, monkeypatch):
    repo = make_repo(tmp_path,
                     dependency_licenses={"left-pad": "MIT", "typescript": "Apache-2.0"},
                     vcpkg_manifest=vcpkg())
    (repo / "cli").mkdir()
    (repo / "cli" / "package.json").write_text(json.dumps({
        "name": "cli", "dependencies": {"left-pad": "^1.3.0"},
        "devDependencies": {"typescript": "~5.5.0"},
    }), encoding="utf-8")
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 0
    npm = {c["name"]: c for c in sbom["components"] if c["purl_ecosystem"] == "npm"}
    assert set(npm) == {"left-pad", "typescript"}  # devDependencies scanned too
    assert npm["left-pad"]["version"] == "^1.3.0"


def test_node_modules_package_jsons_are_skipped(tmp_path, monkeypatch):
    repo = make_repo(tmp_path, dependency_licenses={}, vcpkg_manifest=vcpkg())
    nm = repo / "node_modules" / "some-dep"
    nm.mkdir(parents=True)
    (nm / "package.json").write_text(json.dumps({
        "name": "some-dep", "dependencies": {"not-scanned": "1.0.0"}}), encoding="utf-8")
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 0  # the unmapped dep inside node_modules/ must NOT be scanned
    assert sbom["components"] == []


# ---------------------------------------------------------------------------
# SBOM shape
# ---------------------------------------------------------------------------


def test_sbom_shape(tmp_path, monkeypatch):
    repo = make_repo(tmp_path,
                     dependency_licenses={"zlib": "Zlib"},
                     vcpkg_manifest=vcpkg(deps=[{"name": "zlib", "version>=": "1.3"}],
                                          name="my-engine", version="9.9.9"))
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 0
    assert sbom["bomFormat"] == "CycloneDX"
    assert sbom["specVersion"] == "1.5"
    assert sbom["metadata"]["component"]["name"] == "my-engine"
    assert sbom["metadata"]["component"]["version"] == "9.9.9"
    assert isinstance(sbom["components"], list) and len(sbom["components"]) == 1
    comp = sbom["components"][0]
    assert comp == {"type": "library", "name": "zlib", "version": "1.3",
                    "purl_ecosystem": "vcpkg", "license": "Zlib"}


def test_sbom_written_even_on_failure(tmp_path, monkeypatch):
    """A failing gate still emits the SBOM (CI uploads it with if: always())."""
    repo = make_repo(tmp_path, dependency_licenses={},
                     vcpkg_manifest=vcpkg(deps=["mystery-lib"]))
    code, sbom = run_gate(repo, monkeypatch)
    assert code == 1
    assert sbom is not None and sbom["components"][0]["name"] == "mystery-lib"


# ---------------------------------------------------------------------------
# Configuration errors: clean failure (exit 2), never a traceback
# ---------------------------------------------------------------------------


def test_malformed_allowlist_is_clean_config_error(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path, allowlist_text="{not json!", vcpkg_manifest=vcpkg())
    code, _ = run_gate(repo, monkeypatch)
    assert code == 2
    assert "ERROR: cannot read" in capsys.readouterr().out


def test_missing_allowlist_is_clean_config_error(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path, vcpkg_manifest=vcpkg())
    (repo / "tools" / "license-allowlist.json").unlink()
    code, _ = run_gate(repo, monkeypatch)
    assert code == 2
    assert "ERROR: cannot read" in capsys.readouterr().out


def test_malformed_vcpkg_manifest_is_clean_config_error(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path, vcpkg_text='{"dependencies": [oops')
    code, _ = run_gate(repo, monkeypatch)
    assert code == 2
    assert "ERROR: cannot read" in capsys.readouterr().out


def test_missing_vcpkg_manifest_is_clean_config_error(tmp_path, monkeypatch, capsys):
    """A missing manifest must be a loud config error, not a vacuous 0-component PASS.

    Regression test: the original gate silently PASSED when src/vcpkg.json was absent
    (nothing scanned, nothing denied) — a deny-by-default hole fixed alongside this suite.
    """
    repo = make_repo(tmp_path, vcpkg_manifest=None)
    code, _ = run_gate(repo, monkeypatch)
    assert code == 2
    assert "vcpkg manifest not found" in capsys.readouterr().out


def test_empty_allowed_licenses_refuses_to_pass(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path, allowed=[], vcpkg_manifest=vcpkg())
    code, _ = run_gate(repo, monkeypatch)
    assert code == 2
    assert "no allowed_licenses" in capsys.readouterr().out


# ---------------------------------------------------------------------------
# Integration: the gate passes against the repo's LIVE manifests
# ---------------------------------------------------------------------------


def test_live_repo_manifests_pass_gate(tmp_path, monkeypatch, capsys):
    """The one test allowed to read the real allowlist + src/vcpkg.json (SBOM goes to tmp)."""
    monkeypatch.setattr(check_licenses, "SBOM_PATH", tmp_path / "sbom.json")
    code = check_licenses.main()
    assert code == 0, capsys.readouterr().out
    sbom = json.loads((tmp_path / "sbom.json").read_text(encoding="utf-8"))
    names = {c["name"] for c in sbom["components"]}
    # The spikes feature deps must be visible to the gate (feature deps are scanned).
    assert {"blake3", "entt", "flecs", "quickjs-ng", "simdjson"} <= names
    assert all(c["license"] != "UNKNOWN" for c in sbom["components"])
