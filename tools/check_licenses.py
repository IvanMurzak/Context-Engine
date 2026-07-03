#!/usr/bin/env python3
"""Dependency-license gate for the Context Engine repository (design lock L-57 / O-7).

DENY-BY-DEFAULT: every declared dependency must have a KNOWN license (recorded in
tools/license-allowlist.json under "dependency_licenses") AND that license must be on the
"allowed_licenses" list. Anything unknown or unlisted fails the build.

Current reach — stated honestly:
  * scans direct dependencies declared in src/vcpkg.json ("dependencies", incl. feature deps);
  * scans direct dependencies in any package.json in the repo (excluding node_modules/);
  * does NOT yet resolve transitive graphs (no lockfiles / vcpkg installs exist yet — the
    transitive scan grows with the first real dependency, per ROADMAP M0);
  * emits a minimal CycloneDX-shaped SBOM (sbom.json) listing the scanned components.

Exit code 0 = gate passed; 1 = violation(s); 2 = configuration error.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from _ci_common import load_json_or_exit

REPO_ROOT = Path(__file__).resolve().parent.parent
ALLOWLIST_PATH = REPO_ROOT / "tools" / "license-allowlist.json"
SBOM_PATH = REPO_ROOT / "sbom.json"

SKIP_DIRS = {"node_modules", "build", "out", "vcpkg_installed", ".git"}


def load_json(path: Path) -> dict:
    return load_json_or_exit(path, tag="license-gate")


def vcpkg_dependencies(manifest: dict) -> list[dict]:
    """Direct deps from a vcpkg manifest: strings or {'name': ...} objects, incl. features."""
    deps: list[dict] = []
    raw = list(manifest.get("dependencies", []))
    for feature in manifest.get("features", {}).values():
        raw.extend(feature.get("dependencies", []))
    for entry in raw:
        if isinstance(entry, str):
            deps.append({"name": entry, "ecosystem": "vcpkg"})
        elif isinstance(entry, dict) and "name" in entry:
            deps.append({"name": entry["name"], "ecosystem": "vcpkg",
                         "version": entry.get("version>=", "")})
    return deps


def npm_dependencies(pkg: dict, source: Path) -> list[dict]:
    """Direct deps from a package.json (dependencies + devDependencies)."""
    deps: list[dict] = []
    for section in ("dependencies", "devDependencies"):
        for name, version in pkg.get(section, {}).items():
            deps.append({"name": name, "ecosystem": "npm",
                         "version": str(version), "source": str(source)})
    return deps


def find_package_jsons(root: Path) -> list[Path]:
    results = []
    for path in root.rglob("package.json"):
        if not any(part in SKIP_DIRS for part in path.parts):
            results.append(path)
    return results


def main() -> int:
    allowlist = load_json(ALLOWLIST_PATH)
    allowed = set(allowlist.get("allowed_licenses", []))
    known = allowlist.get("dependency_licenses", {})
    if not allowed:
        print("[license-gate] ERROR: allowlist has no allowed_licenses — refusing to pass.")
        return 2

    components: list[dict] = []
    vcpkg_manifest_path = REPO_ROOT / "src" / "vcpkg.json"
    if not vcpkg_manifest_path.is_file():
        # Deny-by-default extends to the manifest itself: a missing manifest must be a loud
        # configuration error, never a vacuous pass with zero scanned components.
        print(f"[license-gate] ERROR: vcpkg manifest not found at {vcpkg_manifest_path} — "
              "refusing to pass with nothing to scan.")
        return 2
    vcpkg_manifest = load_json(vcpkg_manifest_path)
    components.extend(vcpkg_dependencies(vcpkg_manifest))
    for pkg_path in find_package_jsons(REPO_ROOT):
        components.extend(npm_dependencies(load_json(pkg_path), pkg_path.relative_to(REPO_ROOT)))

    violations: list[str] = []
    for comp in components:
        license_id = known.get(comp["name"])
        comp["license"] = license_id or "UNKNOWN"
        if license_id is None:
            violations.append(
                f"{comp['ecosystem']}:{comp['name']} — license UNKNOWN "
                f"(not recorded in {ALLOWLIST_PATH.name}; deny-by-default)")
        elif license_id not in allowed:
            violations.append(
                f"{comp['ecosystem']}:{comp['name']} — license '{license_id}' "
                f"is not on the allowlist")

    # Minimal CycloneDX-shaped SBOM (grows into a full CycloneDX document with real deps).
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": vcpkg_manifest.get("name", "context-engine"),
                "version": vcpkg_manifest.get("version", "0.0.0"),
            }
        },
        "components": [
            {
                "type": "library",
                "name": c["name"],
                "version": c.get("version", ""),
                "purl_ecosystem": c["ecosystem"],
                "license": c["license"],
            }
            for c in components
        ],
    }
    SBOM_PATH.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")
    print(f"[license-gate] scanned {len(components)} declared dependencies "
          f"(src/vcpkg.json + package.json files); SBOM written to {SBOM_PATH.name}")

    if violations:
        print(f"[license-gate] FAIL — {len(violations)} violation(s):")
        for v in violations:
            print(f"  - {v}")
        return 1

    print("[license-gate] PASS — all declared dependencies have allowlisted licenses.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
