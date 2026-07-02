# tools/

Repository and CI tooling — not engine code. Currently: `check_licenses.py`, the
**deny-by-default dependency-license gate** (scans `vcpkg.json` and any `package.json` against
`license-allowlist.json`, fails on unknown/unlisted licenses, emits a minimal CycloneDX
`sbom.json`), required in CI from the first commit. Governed by the Context Engine design
records: **DESIGN-DECISIONS.md lock L-57** (license allowlist + SBOM, O-7) and **ROADMAP.md §1
M0**.
