---
id: e12-macos-linux-shells
title: macOS + Linux shell backends — NSWindow/IOSurface; X11 + software-OSR behind the L-41 gate (D21)
group: B
sequence: 6
repo: "."
base_branch: "main"
depends_on: [e04-window-shell-windows]
importance: 7
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [03, 01, 07]
---

## Goal

Extend the Shell to the other two OSes behind the same interface: macOS (NSWindow/NSView,
app-bundle CEF model, IOSurface OSR via stock accessors) and Linux (X11 + XWayland, D21;
software-OSR default, acceleration only behind the Mesa/X11-ozone gate) — preserving the
single-platform-`#if` discipline.

## Scope & seams

- **One interface, three impls** — mirror `compositor/surface.cpp`'s single platform `#if`
  discipline (`surface.h:16-18`, `surface.cpp:43-52`); no platform conditionals sprayed
  through shared shell code.
- **macOS**: NSWindow + NSView host; CEF via `CefScopedLibraryLoader` + the existing
  `.app`+helpers+framework packaging model (`host/CMakeLists.txt:61-113`); OSR delivery =
  raw IOSurface → Metal blit through e03's STOCK native accessors; CEF-internal pacing only —
  never `SendExternalBeginFrame` (`surface.cpp:39`, cef#4033); per-backing-scale DPI;
  CPU present fallback via `CALayer.contents`.
- **Linux**: X11 (+XWayland) window + event loop per D21 (native Wayland post-M9);
  software-OSR default (e03 upload path); accelerated dmabuf ONLY behind the Mesa/X11-ozone
  gate as encoded in `select_mode` (`surface.cpp:8-30`) — ships OFF unless the gate proves
  it; CPU present fallback via X11 SHM; DPI from Xft/monitor info.
- **Input pumps**: per-OS normalize → the SAME region-arbitration path built in e04 (03 §6);
  IME basics on both OSes routed to CEF.
- Window placement/persistence, popup suppression, PET_POPUP layer, resize protocol — reuse
  the e04 mechanisms verbatim; only the native edges differ.

## Definition of Done

- [ ] Editor boots windowed with live panels on the macOS GH runner and under Linux xvfb
      (the `ci.yml:1785-1788` pattern)
- [ ] T2 scenario legs green on both OSes (boot, dock, command-driven smoke, tear-out)
- [ ] Software-OSR + CPU-fallback paths verified per OS; Linux accel stays OFF without the
      gate (assert)
- [ ] No `SendExternalBeginFrame` anywhere (grep gate); single-`#if` discipline upheld
      (review check)
- [ ] 3-OS CI green including the new legs
