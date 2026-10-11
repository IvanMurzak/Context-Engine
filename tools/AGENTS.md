# tools/ — repo and CI tooling (Python, not engine code)

- `check_*.py` — CI gates (licenses, CI gating, fleet manifest, toolchain, source pins, include graph, a11y/webui/boundary checks)
- `fetch_*.py` + `*-prebuilt.json` / `*-toolchain.json` / `*-source.json` — SHA-pinned, verify-before-use, fail-closed fetchers and their pins
- `license-allowlist.json` — the deny-by-default license map; `trust-root/` — pinned Ed25519 release signers (`docs/signing.md`)
- `gen_*.py` — generators for committed artifacts; `tests/` — pytest for every tool (plus `fixtures/`, test-only signing material)

## Area rules

- **A from-source configure-time fetch must not be single-sourced (#359).** Pinned callers use
  `context_download_from_pin(PIN <tools/*-source.json> PATH <dest>)` (`cmake/ContextDownload.cmake`),
  which tries `url` then the ordered `mirrors`, re-checking the same `sha256` after each attempt and
  failing closed (R-SEC-009). Before listing a mirror, fetch it and verify it byte-identical; prefer
  an independent domain; re-verify every mirror on each version bump. `check_source_pins.py` checks
  pin structure offline but cannot detect a mirror that has since died — that stays the author's duty.
- Only the FreeType/HarfBuzz pins are mirrored today. V8, esbuild, tsgo and dockview are still
  unconditional single-source configure-time fetches (three via `registry.npmjs.org`) — a known
  unmitigated #359 exposure. Fixing it needs a shared multi-source downloader for the `fetch_*.py`
  family (each carries a private `_download_with_retry`). Toggle-gated prebuilts (wgpu-native, CEF,
  wasmtime, spikes) are accepted exceptions.
