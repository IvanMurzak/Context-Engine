# spikes/cef-compositing — CEF accelerated-OSR / shared-texture compositing (L-41)

The **fifth M0 spike**: prove the editor's riskiest integration seam — CEF UI composited over an
engine-rendered 3D viewport via **accelerated offscreen rendering + shared textures** — before M1
commits the architecture and M5 builds the editor shell on it. Design frame:
`DESIGN-DECISIONS.md` **L-41** (with its ordered fail-on-platform fallback tree), **L-15**,
`REQUIREMENTS.md` **R-UI-007**. THROWAWAY proof code, never production code.

**Verdict + measurements + the per-platform fallback recommendation: [FINDINGS.md](FINDINGS.md).**

## What the demo does (Windows leg — the MUST platform)

One executable, `cef-compositing-spike`:

- Engine-owned Win32 window; **D3D11** flip-model swapchain rendering a spinning triangle (the
  stand-in "viewport").
- **CEF 149.0.6** (stable channel, pinned; see CMakeLists) in windowless mode with
  `shared_texture_enabled=1`: Chromium composites the UI GPU-side and hands over each frame as a
  **D3D11 NT shared handle** via `CefRenderHandler::OnAcceleratedPaint`; the engine opens it
  (`ID3D11Device1::OpenSharedResource1`, cached per pool handle), `CopyResource`s into a private
  texture, and alpha-blends it (premultiplied) over the viewport every frame.
- `--software` switches the same page to the classic **software-OSR** path (`OnPaint` BGRA
  buffer → `UpdateSubresource` with dirty rects) so the fallback-cost delta is *measured*.
- **Self-driving autotest** (always on): synthetic `WM_*` messages are sent through the real
  `WndProc` → `SendMouse*/SendKeyEvent` forwarding path — hover, click, keypress, a second
  click after resize — verified two ways: `document.title` events timestamped native-side
  (input → JS round-trip) **and pixel readback from the composited UI texture** (input → visible
  pixels). Two window resizes (grow + shrink) verify viewport + CEF re-layout.
- Auto-closes after ~12 s (hard 30 s cap) and prints a one-line `RESULT: {...}` JSON with
  fps / present-to-present percentiles / UI paint rate / per-paint CPU cost / latencies /
  pixel-check verdict / resize convergence. Composite proof BMPs are dumped next to the exe.

## Build & run (Windows x64, MSVC, Release only — the minimal CEF distro has no Debug libs)

```bat
cmake -S src -B src/build/cef -DCONTEXT_BUILD_SPIKE_CEF=ON
cmake --build src/build/cef --config Release --target cef-compositing-spike
src\build\cef\spikes\cef-compositing\Release\cef-compositing-spike.exe            & rem accelerated, vsync
src\build\cef\spikes\cef-compositing\Release\cef-compositing-spike.exe --novsync  & rem accelerated, uncapped
src\build\cef\spikes\cef-compositing\Release\cef-compositing-spike.exe --software & rem software-OSR fallback
```

Flags: `--software`, `--novsync`, `--windowless-fps N` (default 60), `--url <url>`.
`-DCONTEXT_SPIKE_CEF_ARCHIVE=<path>` reuses a pre-downloaded distribution archive.

## CI posture

**Never built in CI** (CEF = ~162 MB download, ~460 MB extracted, and the proof needs a GPU +
interactive desktop). Doubly gated: `CONTEXT_BUILD_SPIKE_CEF` (OFF) on top of
`CONTEXT_BUILD_SPIKES`. The CI bench job (spikes ON, CEF OFF) still *enters* this directory and
takes the early `return()` — keeping the spike's CMake configure-valid on every push — see the
comment in `.github/workflows/ci.yml`.
