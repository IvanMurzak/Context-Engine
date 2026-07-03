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
- `verify_artifact.py` — the **R-SEC-009 verify-before-use artifact gate**: authenticates an
  artifact against a **detached Ed25519 signature** and the **pinned trust root**
  (`trust-root/allowed_signers`), **failing closed** on a missing/bad/tampered/untrusted-key
  signature. Ed25519 only, via `ssh-keygen -Y` (OpenSSH — zero third-party Python deps). The
  production key is minted at the first release; the procedure + custody options live in
  `docs/signing.md`. Governed by **DESIGN-DECISIONS.md lock L-58 / R-SEC-009**.

Sub-directories:

- `trust-root/allowed_signers` — the pinned production trust root (empty until first release —
  see `docs/signing.md`); `verify_artifact.py` checks every artifact against it.
- `tests/fixtures/` — TEST-ONLY signing material for `test_verify_artifact.py`: a public key,
  its pinned `allowed_signers`, a sample artifact, and a pre-made detached signature. **No
  private key is committed** — cases needing the private half mint an ephemeral key at test time.

Tests live in `tools/tests/` (R-QA-013) and run in the CI `python-tests` job. Governed by the
Context Engine design records and **ROADMAP.md §1 M0**.
