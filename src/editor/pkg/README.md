# `src/editor/pkg/` — engine-driven package install (R-SEC-005)

The security envelope for the TS scripting tier's package installs (issue #100, ROADMAP §1 M3:
"engine-driven npm installs land with the TS toolchain here under the R-SEC-005 rules"). This is
**not** a full npm client — it is the *supply-chain gate* around installing a package's bytes, with a
pluggable artifact source so every invariant is TESTED, not merely configured.

## The invariants (all fail-closed)

1. **`--ignore-scripts` in ALL tiers, by construction.** Extraction (`npm_install.cpp`) writes files
   verbatim and executes nothing — there is **no code path that spawns a shell/subprocess** — so a
   package's `preinstall`/`install`/`postinstall` lifecycle scripts can never run from an
   engine-driven install. `test_npm_deps` proves a package carrying a `postinstall` installs with the
   script's side-effect absent (and the script text preserved, not stripped — we simply never run it).
2. **Pinned versions + lockfile integrity, incl. transitive** (`lockfile.h`). Every root dependency
   spec must be an exact SemVer pin (a range / dist-tag / url is `install.version_unpinned`), and
   every entry in the npm v3 `package-lock.json` `packages` map — direct *and* transitive — must carry
   an exact version + a non-empty `integrity` (else `install.lockfile_incomplete`). Each fetched
   artifact's bytes are verified against its SRI (`integrity.h`) before use; a mismatch is
   `install.integrity_mismatch` and the artifact is **refused, never used with a warning** (R-SEC-009).
3. **Scripts-requiring ⇒ native-tier ⇒ L-49 consent gate.** A lockfile entry flagged
   `hasInstallScript` classifies the package **native-tier**; v1 carries no native-consent grant
   (R-SEC-001: no third-party native packages), so it is refused fail-closed with
   `install.scripts_required` (the reserved R-SEC-011 `consent_required` code names the general async
   consent protocol). The gate short-circuits the WHOLE plan before a byte is extracted.

The R-CLI-008 codes are DEFINED here (`codes.h`, `context::editor::pkg::kInstall*Code` /
`kConsentRequiredCode`) and REGISTERED in `src/editor/contract/error_catalog.cpp` — the
promote-a-local-string pattern (like runtime/ts's `kTs*Code`), so this module needs no back-edge to
the contract layer.

## Layers

| File | Role |
|------|------|
| `sha512.{h,cpp}` / `base64.{h,cpp}` | Stdlib-only crypto for SRI (no license-gate dependency). |
| `integrity.{h,cpp}` | SRI (`<alg>-<base64>`) verify — sha512 (npm default) + sha256; strongest-present authoritative; fail-closed. |
| `tar.{h,cpp}` | Minimal POSIX ustar reader/writer (uncompressed). |
| `lockfile.{h,cpp}` | `package.json` + `package-lock.json` v3 parse + the pin/completeness gate. |
| `npm_install.{h,cpp}` | `plan_install` (validate + classify) → `execute_install` (fetch + SRI-verify + extract-no-scripts) over a pluggable `PackageSource`. |

The CLI verb is `context install` (`src/cli/install_command.cpp`); it is a STABLE one-shot verb in the
ONE registry, so CLI ≡ RPC ≡ MCP ≡ introspection parity holds by construction (R-CLI-009).

## v1 seams (documented, NOT built — the issue's HALT-split boundary)

- **Live-registry fetcher.** v1 ships an *offline* `PackageSource` (`--source <cache-dir>` reads
  plain-ustar artifacts). The TLS/cert-pinned live npm-registry fetcher + gzip `.tgz` decompression
  (npm serves `gzip(ustar)`; SRI is over the `.tgz`) is a tracked follow-up. The SECURITY MECHANISM
  (SRI verify, pin, scripts gate, extract-no-scripts) is algorithm/transport-agnostic and lands now.
- **npm provenance / attestation surfacing** (R-SEC-005 SHOULD) — the designated HALT-split; the SRI
  `integrity.h` seam is where provenance verification will attach.
- **vcpkg from-source native builds** (L-42 / R-SEC-010 build-env jail) — explicitly out of scope; v1
  has no third-party native packages (R-SEC-001), so the jail is SHOULD and unbuilt.
