# tools/

Repository and CI tooling — not engine code. Currently:

- `check_licenses.py` — the **deny-by-default dependency-license gate** (scans `src/vcpkg.json`
  and any `package.json` against `license-allowlist.json`, fails on unknown/unlisted licenses,
  emits a minimal CycloneDX `sbom.json`), required in CI from the first commit. Governed by
  **DESIGN-DECISIONS.md lock L-57** (license allowlist + SBOM, O-7).
- `check_toolchain.py` — the **L-42 pinned-toolchain manifest gate**: reads
  `cmake/toolchain-versions.json` (the declared target-vs-actual toolchain record) and gives CI
  its pin queries (`--print-pin`, `--print-install-major`), version verification (`--verify`
  with strict/advisory/documented enforcement), and the `--describe` summary. Applied per CI
  job by the `.github/actions/pinned-toolchain` composite action. Governed by
  **DESIGN-DECISIONS.md lock L-42**.

Tests live in `tools/tests/` (R-QA-013) and run in the CI `python-tests` job. Governed by the
Context Engine design records and **ROADMAP.md §1 M0**.
