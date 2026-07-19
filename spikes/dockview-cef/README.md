# spikes/dockview-cef — M9 spike s1 (THROWAWAY)

Ratifies design decision **D2** (Dockview v7 as the M9 editor docking engine) under real conditions:
CEF 149, a strict no-inline-script CSP served from a **custom scheme** (`context-editor://`, not
`file://`), and **sandboxed-iframe** panel content. **Throwaway proof code — the ratified DECISIONS
are the deliverable**, recorded in [`FINDINGS.md`](FINDINGS.md) + [`supply-chain-review.md`](supply-chain-review.md).
No production target links this spike; it is default-OFF and never built in CI.

Verdict: **RATIFY** — see `FINDINGS.md`. Pin **`dockview-core@7.0.2`** (MIT, zero runtime deps); v7
ships all feature modules (accessibility included) inside that one package.

## Layout

```
spikes/dockview-cef/
  FINDINGS.md              the 6-probe PASS/FAIL matrix + ratification (primary deliverable)
  supply-chain-review.md   the npm allowlist-gate artifact (08 §3)
  web/                     the harness both tiers load
    index.html             strict no-inline-script CSP; loads the vendored bundle + probes
    probes.js              the 6-probe matrix runner (emits a JSON verdict 3 ways)
    panel.html / panel.js  sandboxed opaque-origin panel content (MessageChannel-port bridge)
    harness.css
    vendor/dockview-core/  pinned dockview-core@7.0.2 UMD + CSS (+ PIN.txt with hashes)
  tools/run_probes.py      Tier-1 headless-Chromium driver + the ctest self-check
  src/main.cpp             Tier-2 CEF-149 host (custom scheme + process-isolation residuals)
  CMakeLists.txt           gating (mirrors spikes/cef-compositing)
```

## Two evidence tiers

CEF 149 **is** Chromium 149, so the Chromium-platform probes (CSP, sandboxed-iframe opaque origin,
Dockview API, popout guard, ARIA) are measured in a plain headless Chromium; only the
custom-scheme + OS-process-count residuals need the CEF host.

- **Tier 1 (measured, any OS + a Chromium-family browser + python3).** Measures probes 1-4 and 6.
  This is also the registered ctest self-check (R-QA-013 spike carve-out):
  ```bash
  python spikes/dockview-cef/tools/run_probes.py
  # or, through CMake/ctest:
  cmake -S src --preset dev -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON
  ctest --preset dev -R context-spike-dockview --output-on-failure
  ```
  `run_probes.py` serves `web/` over `http://127.0.0.1`, opens it headless, and collects the JSON
  verdict the harness POSTs to `/done`. `--allow-skip` exits 0 (SKIP) where no browser is found.

- **Tier 2 (Windows/MSVC only, local — NEVER in CI).** Adds the `context-editor://` custom-scheme
  serving (CSP as a real response header) + the renderer-process-count probe. Doubly-gated behind
  `CONTEXT_BUILD_SPIKE_DOCKVIEW`; downloads the same pinned ~162 MB CEF 149 as
  `spikes/cef-compositing`:
  ```bash
  cmake -S src -B src/build/dvspike -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON
  cmake --build src/build/dvspike --config Release --target dockview-cef-spike
  src/build/dvspike/spikes/dockview-cef/Release/dockview-cef-spike.exe   # writes findings-verdict.json
  ```

## Gating (CI stays untouched)

With `CONTEXT_BUILD_SPIKE_DOCKVIEW=OFF` (the default, and the CI bench job's
`CONTEXT_BUILD_SPIKES=ON` configure) the directory is entered and **returns early registering
nothing** — identical to `spikes/cef-compositing`. With the switch ON, the python probe self-check is
registered on any toolchain, and the CEF host is built only on Windows+MSVC. The CEF binary is never
downloaded in CI. See `../README.md` and the design decision D2 in
`.claude/design/context-engine/m9-editor/04-editor-web-app-docking-panels.md`.
