---
id: s2-wgpu-shared-texture-spike
title: Patched wgpu-native prebuilt — DXGI shared-handle import over create_texture_from_hal, sandbox ON; CPU-upload fallback measured
group: B
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 9
security_critical: true   # new patch-carrying pinned prebuilt = supply-chain change class
production_touching: false
model_hint: top
taskflow_refs: [01, 03, 08]
---

## Goal

Prove the Windows accelerated-OSR path re-based by review B-F1: a patched, pinned wgpu-native
v29-line prebuilt exposing a small C entry over wgpu's Rust-core `create_texture_from_hal`
that imports CEF's DXGI NT shared handle on the D3D12 backend — under CEF **sandbox ON** +
`shared_texture_enabled` + windowless — and measure the stock-API CPU-upload fallback.

## Scope & seams

- **The patch**: fork the wgpu-native v29 line (same line as the existing `v29.0.1.1` pin,
  `src/render/CMakeLists.txt`, `docs/native-webgpu-backend-decision.md:13,128`); add a C entry
  wrapping `create_texture_from_hal` (D3D12 backend; reference: wgpu PR #6161 wired D3D11
  shared handles for Vulkan). Keep the patch minimal and rebasable.
- **Prebuilt supply chain**: SHA-pinned prebuilt per the repo convention —
  `tools/*-prebuilt.json` + `fetch_*.py` + `cmake/ContextDownload.cmake` (the CEF/V8/wgpu
  precedent). ⚠ New pinned prebuilt/fork = the 08 §3 standing consent gate (owner approval
  BEFORE it lands).
- **The probe** (throwaway, `spikes/` — extend `spikes/cef-compositing/`):
  - CEF 149 `OnAcceleratedPaint` NT shared handle → patched import → wgpu texture →
    premultiplied composite (`renderer_d3d11.cpp:294-331` / `:210-272` reference behavior,
    now through wgpu instead of raw D3D11).
  - **Sandbox ON** + `shared_texture_enabled` + windowless asserted together (regression
    precedent cef#4057 — B-F5); record behavior for the per-CEF-bump tripwire design.
  - Handle discipline (B-F10): contract-compliant reopen-per-paint as default; the measured
    per-handle-value cache (~11-deep pool) behind a flag.
  - **CPU-upload fallback measured**: `OnPaint` BGRA → `queue.writeTexture` with dirty rects
    (`updateUiFromBuffer` pattern); per-paint CPU cost vs accelerated (spike baseline:
    accel ≈27 µs vs software ≈114 µs).

## Definition of Done

- [ ] Owner consent obtained for the patched-fork prebuilt (08 §3 standing gate)
- [ ] Patched prebuilt builds reproducibly; SHA-256-pinned via `tools/*-prebuilt.json` +
      fetch-verify fail-closed
- [ ] Import proven: CEF shared texture composited through wgpu with sandbox ON +
      `shared_texture_enabled` (evidence in FINDINGS.md)
- [ ] CPU-upload fallback measured and recorded; flag-switch between paths demonstrated
- [ ] Go/no-go findings for e03 recorded (incl. tripwire + auto-degrade recommendation);
      ROADMAP progress log updated by the TD
