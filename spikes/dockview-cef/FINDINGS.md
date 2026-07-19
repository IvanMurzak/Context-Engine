# FINDINGS — Dockview-in-CEF ratification spike (M9 s1)

THROWAWAY spike. The ratified **DECISIONS** below (+ the [supply-chain review](supply-chain-review.md))
are the deliverable, not the harness code. Ratifies design decision **D2** (Dockview v7 as the M9
editor docking engine) under real conditions — strict no-inline-script CSP, a custom scheme (not
`file://`), and sandboxed-iframe panel content — per `04-editor-web-app-docking-panels.md` §2 and
`08-security.md` §§1-3.

## Verdict: **RATIFY D2 — Dockview v7 (`dockview-core@7.0.2`)**

Every probe that can be measured in a real Chromium engine PASSED; probe 5 (OS renderer-process
isolation) is a CEF-host residual with the DOM-level precondition confirmed. **No fallback is
triggered** — the Golden Layout -> Lumino path (`04` §2, D2 order) stays unused. One correction to
the design's assumption is recorded (§ Package set): the "`dockview` core + `dockview-modules` incl.
an a11y module" split **does not exist as separate npm packages** — v7 ships all feature modules
(accessibility included) inside `dockview-core`, auto-registered. The pin is therefore **one MIT,
zero-runtime-dependency package**.

## Probe matrix

| # | Probe | Result | Evidence tier |
|---|-------|--------|---------------|
| 1 | Docking core (splits/tabs/floating) under strict no-inline-script CSP + non-`file://` origin | **PASS** | measured (Chromium) + CEF residual for custom scheme |
| 2 | Sandboxed-iframe panel content (`allow-scripts`, opaque origin) — layout/resize/focus | **PASS** | measured (Chromium) |
| 3 | `toJSON()` / `fromJSON()` serialize-restore fidelity incl. floating groups | **PASS** | measured (Chromium) |
| 4 | v7 rejects non-`http(s)` popout URLs (popout API deliberately unused) | **PASS** | source-verified + measured |
| 5 | Per-extension process isolation (`IsolateSandboxedIframes`) under CEF 149 | **RESIDUAL** | DOM precondition measured; OS proof = CEF host |
| 6 | a11y scan of Dockview chrome (tabs, drop zones) — feeds e16 | **PASS** | measured (Chromium) |

## Measurement methodology

Two evidence tiers, because CEF 149 **is** Chromium 149 (`149.0.7827.201`, per
`tools/cef-prebuilt.json`) and the probed behaviors are Chromium-platform behaviors, not CEF-API
behaviors:

- **Tier 1 — measured this run (`tools/run_probes.py`, headless Chromium).** The `web/` harness runs
  the 6 probes and POSTs a JSON verdict back (the `tools/web_golden_run.py` POST-back pattern). This
  run measured on **HeadlessChrome/150** (Edge 150 — Chromium **one major above** the CEF-149 pin;
  the CSP / sandboxed-iframe / roving-tabindex / Dockview-API / popout-guard behaviors are stable
  across 149<->150). Reproduce: `python tools/run_probes.py`. This is also the registered ctest
  self-check `context-spike-dockview-probes` (R-QA-013 spike carve-out).
- **Tier 2 — authored + local-MSVC-run (`src/main.cpp`, CEF 149 host).** Reuses
  `spikes/cef-compositing`'s CEF-149 lifecycle; serves the SAME harness from the pinned custom
  scheme `context-editor://` (`STANDARD|SECURE|CORS_ENABLED`) with the CSP as a real response
  header, and counts renderer subprocesses for probe 5. Like `cef-compositing` it is **doubly-gated,
  Windows/MSVC-only, and never built in CI**; its run is a local Windows-dev-box step (the CEF binary
  is a ~162 MB pinned download). The custom-scheme serving + OS-process count are its residual
  contribution; everything else is already proven in Tier 1.

CSP under test (the security-critical directive is `script-src 'self'` — no `unsafe-inline`, no
`unsafe-eval`): `default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src
'self' data:; font-src 'self'; connect-src 'self'; frame-src 'self'; child-src 'self'; base-uri
'none'; form-action 'none'`. Delivered by `<meta http-equiv>` (Tier 1) AND as a real
`Content-Security-Policy` response header from the scheme handler (Tier 2).

## Per-probe detail (measured values)

**Probe 1 — docking core under strict CSP + non-`file://` origin — PASS.** Built a layout with a
horizontal split, a tabbed group, and a floating group: `panels=4`, `groups=3`, `floatingGroups=1`.
**`0` CSP violations of any directive** (`securitypolicyviolation` listener) — Dockview v7 runs under
the strict no-inline-script policy with **no `unsafe-eval`** (confirmed: the bundle contains no
`eval(` / `new Function(`). Styles need `style-src 'unsafe-inline'` (the library injects a runtime
`<style>` and inline positioning); scripts do not — the security-relevant half holds. Origin was
`http://127.0.0.1` (a real non-`file://` origin); the `context-editor://` secure-origin equivalent is
the Tier-2 residual (custom schemes registered `SECURE` are treated as trustworthy origins, so CSP
enforcement is identical).

**Probe 2 — sandboxed-iframe panel — PASS.** Mounted a panel whose content is
`<iframe sandbox="allow-scripts">` (no `allow-same-origin` => opaque origin). Measured:
`opaqueOrigin(window.origin==="null")=true`, `parentDomBlocked=true` (the frame cannot read
`parent.document`), `resizePropagated=true` (panel resize 300->220px propagated to the iframe). The
bridge is a **MessageChannel port** handed to the frame at creation — it authenticates by holding
the port, never by origin string (origin is `"null"`), exactly per `04` §5 / `08`. The port round-trip
delivered the frame's readiness message.

**Probe 3 — serialize/restore fidelity — PASS.** `toJSON()` -> `clear()` (verified emptied to 0) ->
`fromJSON()`: `panels 5->5`, `groups 3->3`, `floatingGroups 1->1`. Floating groups survive the round
trip. (The layout carried the probe-2 iframe panel, hence 5.) v7 API: `createDockview(el, {
createComponent, theme })`, `api.toJSON() / fromJSON() / clear()`, `SerializedDockview.floatingGroups`.

**Probe 4 — non-`http(s)` popout URL rejected — PASS.** Called `api.addPopoutGroup(group, {
popoutUrl: "context-editor://app/popout.html" })`: **no popout was created** (`popoutOpened=false`).
Source-verified against the pinned bundle — `popoutWindow.js::assertSameOriginPopoutUrl(url)` runs
**before** `window.open` and throws for anything whose protocol is not `http:`/`https:` or whose
origin differs (source comment: "Reject popout URLs that aren't same-origin http(s). Blocks
`javascript:`, `data:`, `blob:`, `vbscript:`, and cross-origin URLs"; guard
`protocolOk = resolved.protocol === 'http:' || resolved.protocol === 'https:'`). Runtime nuance: in
7.0.2 `addPopoutGroup` surfaces the rejection as a **resolved `false`** (guarded no-op), not a thrown
rejection. Either way the security outcome holds: **a `context-editor://` (custom scheme) popout can
never open**, which is exactly why the design keeps Dockview's popout API unused and drives tear-out
through the Shell/PanelHost instead (`04` §2, B-F2). Confirmed.

**Probe 5 — per-extension process isolation — RESIDUAL (CEF host).** OS renderer-process boundaries
are not observable from page JS. Tier 1 confirms the **necessary DOM precondition**: distinct
opaque-origin sandboxed frames are mutually and parent isolated (`crossFrameBlocked=true`). The
**sufficient** proof — that N distinct-origin (`context-ext://<id>`) sandboxed iframes map to N
separate renderer OS processes — is measured by the Tier-2 CEF host (`childProcessCount()` at verdict
time). **`IsolateSandboxedIframes` is a Chromium feature DEFAULT, not a CEF contract** (`08` threat
table); the CEF host records the actual process count observed under CEF 149 rather than assuming it.
This is the one probe whose full proof requires the local-MSVC CEF run.

**Probe 6 — a11y scan of Dockview chrome — PASS (feeds e16).** Scan of the live chrome:
`role=tablist:3`, `role=tab:5`, `role=tabpanel:3`, plus `region:3`, `dialog:1` (floating group),
`status:1`, `alert:1`; **2 `aria-live` regions** (the built-in accessibility module's LiveRegion);
all 5 tabs have accessible names. Tab focus uses the **WAI-ARIA roving-tabindex pattern**
(`focusableTabs=3` = one tab per tablist in the tab order, siblings arrow-key reachable) — the
*preferred* (more accessible) tablist pattern, not a gap. Dockview v7 ships a genuine ARIA surface;
this feeds the e16 axe-class scan scope. (Drop-zone `.dv-drop-target` elements are created during an
active drag and are out of scope for a static scan; e16 exercises them under a driven drag.)

## Package set (B-F9) — RATIFIED, with a correction to the design assumption

**Pin exactly one package: `dockview-core@7.0.2`.** It is the framework-agnostic core with the
vanilla `createDockview()` entrypoint — the right fit for a no-framework CEF web app.

The design's phrasing "`dockview` core + `dockview-modules` (incl. the a11y module), exact pinned
versions" **is corrected by this spike**:

- There is **no `dockview-modules` npm package** and **no separate accessibility package**. v7's
  module system is *internal* to `dockview-core`: `AllModules` (accessibility, floating-group,
  popout, keyboard-docking, live-region, context-menu, tab-group-chips, advanced-DnD, watermark,
  edge-group, root-drop-target, header-actions) are **auto-registered by `DockviewComponent` at
  construction**. The bundle's own source says the per-component `modules` option and external module
  authoring API are *"reserved for a future version ... remains internal."* So in 7.0.2 every module,
  **accessibility included, is on by default** — nothing to install or pin separately.
- `dockview` (the framework binding, depends on `dockview-core`), `dockview-react`, `dockview-vue`,
  `dockview-angular` are **NOT needed** — they add a UI-framework layer the CEF editor-core does not
  use. Do not pin them.
- Styles: ship `dockview-core/dist/styles/dockview.css` (bundles the built-in theme classes
  `dockview-theme-dark`/`-light`/... + `--dv-*` CSS custom properties; `04` §2 CSS-variable theming).
  The design's own Dark/Light/HC tokens (`06`) override the `--dv-*` variables.

### Pinned versions + verify-before-use hashes

| Artifact | Pin |
|---|---|
| npm package | `dockview-core@7.0.2` (MIT, **0 runtime deps**, published 2026-06-22) |
| npm integrity | `sha512-i666ndkUdT4U+Bo2Yu3d6KwCJNqe21VJ0oschdgcXucZ5TquzBFX94zcUBEfg4IewTv2BuiPJ7r/2JTGLLv6ig==` |
| tarball sha256 | `e75f201ed9a59346274a333cfcebc56cccf5b11a576b008910cb063ed0ecf5eb` |
| `dockview-core.min.js` sha256 | `ac39da12d4280a2c61a55b98c66faa110ed3c2c93457e57f30d6e0e369e33eff` |
| `dockview.css` sha256 | `4cc8d24e797c4a8505a11060094b01cbc27af685d3fdef9b04c7d8d5716468ac` |
| UMD global | `window["dockview-core"]` |

The production build path uses the SHA-pinned esbuild (`tools/ts-toolchain.json`) to bundle
`dockview-core` into the editor-core static assets (`04` §1: no Node at runtime, no npm in CI). The
spike vendors the pre-built UMD + CSS under `web/vendor/dockview-core/` (see its `PIN.txt`) so it is
offline and adds **no `package.json`** — the deny-by-default `license-gate` (which scans every
`package.json` in the repo) stays untouched and the owner's s1 allowlist ratification is not
pre-empted. The allowlist recommendation is in the [supply-chain review](supply-chain-review.md).

## Fallback assessment

**Not triggered.** All measurable probes passed and the package/licensing/supply-chain posture is
clean (one MIT, zero-dependency package). The `04` §2 fallback order — Golden Layout, then Lumino +
our own layer — is **not needed**; recorded here only as the standing contingency if a future
CEF/Chromium upgrade regresses a probe.

## Reproduce

```bash
# Tier 1 (any OS with a Chromium-family browser + python3) — measures probes 1-4, 6:
python spikes/dockview-cef/tools/run_probes.py            # or: ctest --preset dev -R context-spike-dockview
# Tier 2 (Windows/MSVC only) — custom-scheme + OS process-isolation residuals:
cmake -S src -B src/build/dvspike -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON
cmake --build src/build/dvspike --config Release --target dockview-cef-spike
src/build/dvspike/spikes/dockview-cef/Release/dockview-cef-spike.exe   # writes findings-verdict.json
```
