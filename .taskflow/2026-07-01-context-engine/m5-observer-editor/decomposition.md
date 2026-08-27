# M5 — Editor GUI & Play-in-Editor (observer-grade): foundation-first task DAG

> Decomposition 2026-07-10 (architect pass, verified against the checked-out CE repo). Feeds M5 task
> files + `implement-task` dispatch. Design authority: `../core/ROADMAP.md` §1 M5 + REQUIREMENTS
> R-EDIT-001 / R-HUX-* / R-A11Y-001 / R-PLAY-* / R-UI-007 / R-OBS-001 + DESIGN-DECISIONS L-41/L-15/L-22/L-51/L-20/L-35.

## Seams & shared merge anchors (verified)
- GUI is a **new client** over existing machinery: `src/editor/bridge/` (JSON-RPC 2.0 + R-BRIDGE-008
  event stream) over the `src/editor/contract/` registry (`registry.cpp`/`handshake.cpp`). CEF host =
  a third client alongside `context editor smoke` + `context attach`. **No new authoring path.**
- **No `src/editor/gui/` exists yet** — M5 is greenfield there.
- Viewport must match the web WebGPU build: `src/render/web/web_main.cpp` + `context_render` /
  `context_render_wgpu`; equivalence via the M4 golden-scene method (`tools/golden_compare.py`, `goldens/`).
- CEF de-risked but **never in CI**: `spikes/cef-compositing/` (L-41 tree ratified), `CONTEXT_BUILD_SPIKE_CEF`,
  ~162 MB download, Windows/MSVC-only. Promoting to a real subsystem + per-OS boot-smoke CI job = net-new infra.
- **Two shared anchors to keep disjoint for 2-at-a-time waves:** (1) `src/CMakeLists.txt` — foundation adds
  ONE `add_subdirectory(editor/gui)` stanza + creates `src/editor/gui/CMakeLists.txt` as a **sub-anchor** so
  panels contend one level down; (2) `src/editor/contract/src/error_catalog.cpp` — append-only tail →
  **≤1 code-minting task per wave**, or pre-reserved domain blocks (`viewport.*`, `play.*`, `gui.*`).

## FOUNDATION (lands SOLO first — pre-split into F0a → F0b to stay under single-pass scale)

### F0a — CEF supply-chain + build substrate  ⟵ FIRST DISPATCH
`tools/fetch_cef.py` + `tools/cef-prebuilt.json` (SHA-pinned prebuilt, modeled on `fetch_v8.py`/`v8-prebuilt.json`)
+ `cmake/ContextDownload.cmake` wiring + verify-before-use (`tools/verify_artifact.py`) + `CONTEXT_BUILD_GUI_CEF`
toggle (promotes `CONTEXT_BUILD_SPIKE_CEF`) + CEF license-allowlist entry + a minimal ctest (fetch→verify→link→
a browser subprocess boots headless & exits). **Ids:** R-SEC-009, L-15, L-42/#76-Option-A carve-out. De-risks
CEF before anything builds on it. Solo.

### F0b — CEF editor host + R-EDIT-001 contract + headless UI-logic tree + L-41 seam + per-OS boot smoke
`src/editor/gui/{host,contract,uitree,compositor}/` + `src/editor/gui/CMakeLists.txt` sub-anchor + one
`add_subdirectory(editor/gui)` in `src/CMakeLists.txt` + new per-OS `editor-cef-smoke` CI job (modeled on
`render:`) + a11y-harness hook + human-loop latency instrumentation seam. Host attaches to `bridge/` via
`handshake.cpp`; capability-scoped bridge shim (read/query default, Node OFF, isolated renderer, strict CSP).
**Ids:** R-EDIT-001, R-UI-007, L-15, L-41, R-BRIDGE-008, R-A11Y-001(seam), R-HUX-011(seam), R-SEC-007, L-20.
Depends F0a. Solo (may itself pre-split into host+smoke / contract+uitree+compositor if over-scale).

## FAN-OUT (after F0; each in its own `src/editor/gui/<subdir>/`, built on R-EDIT-001, ships per-panel a11y + latency)
- **F1 native viewport (3D+2D)** — `gui/viewport/`, links `context_render(_wgpu)`; golden-scene equivalence vs web. **Mints `viewport.*`.** Deps F0. *Highest risk (GPU/compositor per-OS) — front-load.*
- **F2 scene-tree panel** — `gui/panels/scenetree/`, read-only over bridge + `derivation.settled`. No new codes. Deps F0.
- **F3 inspector (override-write edits)** — `gui/panels/inspector/`; edits = L-35 overrides via the `context set` write path, CAS-guarded, gesture-end commit (L-20). **Reuses existing codes** (`cas.mismatch`, `compose.*`). Deps F0 (+F2 loose).
- **F4 Problems panel + inline markers** — `gui/panels/problems/`, read-only; R-BRIDGE-008 stability-aware. No new codes. Deps F0.
- **F5 play controls / play-in-editor** — `gui/playbar/` + session-control over `src/runtime/session/`; L-51 live-edit split, L-22 hot reload. **Mints `play.*`.** Deps F0 **+ F1** (rendered play output).
- **F6 a11y scan + keyboard-nav harness** — `gui/a11y/` + `tools/a11y_scan.py` + CI steps; per-panel coverage manifest (shared append anchor). No codes. Deps F0. *Land early so F2–F5 gate against it.*
- **F7 GUI session undo/redo** — `gui/session/undo/`; replays through the serialized write queue + `--if-match` CAS + L-30 rebase-or-drop; `.editor/session.json`. Reuses `cas.mismatch`. Deps F0 **+ F3**.

## Dispatch order (2-at-a-time; each pair directory-disjoint + ≤1 code-minter)
```
SOLO   F0a → F0b        (foundation; green before any fan-out)
WAVE1  F6 (tools/CI, no codes)      ∥ F1 (viewport, mints viewport.*)     ← front-load GPU risk
WAVE2  F2 (scenetree, no codes)     ∥ F4 (problems, no codes)            ← two pure observers
WAVE3  F3 (inspector, reuses codes) ∥ F5 (playbar, mints play.*)         ← one minter each; F5 needs F1
WAVE4  F7 (undo, needs F3)          ∥ (optional) R-HUX-004 command palette (gui/palette/)
```

## M5 EXIT
Open a project → inspect (scene tree F2 + inspector F3 + Problems F4) → play it (F5) → make an override edit
through the inspector (F3, undoable F7) → viewport (F1) matches a web WebGPU build within the T1 feature set →
per-OS CEF boot smoke + automated a11y scan + keyboard-only nav green on every shipped panel; R-HUX-011
human-loop latency measured (SHOULD exit measurement).

## OUT of M5 (do not over-scope)
Asset browser (R-HUX-007 → v1.x) · tile-painting GUI + 2D authoring (R-2D-003 → M8.5) · in-context override
*viewport* editing (R-HUX-006 → M8.5; M5 = inspector-override edits only) · contextual help (R-HUX-010 → M8.5) ·
git-history timeline (R-HUX-002) + launcher (R-HUX-003) → v2 · visual scene-merge (R-HUX-008, SHOULD, slips) ·
third-party extension red-teaming → v2 (v1 = design-time contract; built-ins-on-the-contract stays MUST).

## Owner-decision gates (surfaced by the decomposition)
1. **CEF supply-chain** — signed-prebuilt narrow carve-out, same class as V8/wgpu-native #76 Option-A →
   **pre-decided by that precedent** (SHA-pin + TLS + verify-before-use under R-SEC-009/L-42). F0a proceeds under it.
2. **CEF sandbox-vs-not packaging** (M138+ Windows `bootstrap.exe` DLL model) — an explicit **during-M5 work
   item**; does NOT touch the L-41 seam → not an F0 blocker; decide before packaging.
3. **CI cost / runner + download budget** — CEF ~162 MB/run + a new per-OS boot-smoke job on the self-hosted
   Windows runners. Cached via the #132 download-hardening; modest + design-accepted (L-15 CEF weight tax).
   **Budget heads-up to owner** — not a hard blocker.
