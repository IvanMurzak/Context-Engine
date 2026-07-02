# spikes/webgpu — WebGPU-baseline rendering spike

**Design records:** L-11/L-56 (WebGPU baseline, WebGPU-only web in v1), R-REND-001/002/005,
R-HEAD-002. **Throwaway proof code, never production code.** Verdict and measurements:
[`FINDINGS.md`](FINDINGS.md).

One triangle through `webgpu.h` semantics, from the same C++ source, on two legs:

- **Native** — [wgpu-native](https://github.com/gfx-rs/wgpu-native) `v29.0.1.1` **prebuilt**
  (pinned, SHA256-verified download at configure time; recorded L-42 deviation — see
  FINDINGS.md; neither Dawn nor wgpu-native has a vcpkg port).
- **Web** — Emscripten + the **emdawnwebgpu** port, binding the **browser's** WebGPU
  (`webgpu.h` → JS shim). This is the locked web path (ARCHITECTURE.md §4), not a Dawn
  cross-compile.

## Modes (`context-spike-webgpu [probe|render|window]`)

| Mode | What it proves | Exit codes |
| --- | --- | --- |
| `probe` | R-HEAD-002 seam: enumerate adapters / report absence **without creating a GPU device** | always 0 |
| `render` (default) | Offscreen triangle → texture→buffer copy → pixel readback → color + analytic-coverage asserts → `PASS` + FNV-1a image hash (`webgpu-spike-hash.txt`) — fully headless, no window/swapchain | 0 pass · 77 skip (no adapter) · 1 fail |
| `window [frames]` | Windowed sanity (surface/swapchain path), Win32-only | 0 · 77 · 1 |

## Build — native

```sh
# standalone (no vcpkg needed; downloads the pinned wgpu-native prebuilt itself)
cmake -S spikes/webgpu -B spikes/webgpu/build-native -DCMAKE_BUILD_TYPE=Release
cmake --build spikes/webgpu/build-native --config Release
ctest --test-dir spikes/webgpu/build-native -C Release --verbose

# or integrated with the other spikes (vcpkg toolchain; from the repo root)
cmake -S src --preset spikes
cmake --build src/build/spikes --config Release --target context-spike-webgpu
```

The readback self-check runs on **software adapters** too: lavapipe/llvmpipe (`apt install
mesa-vulkan-drivers`) on Linux, WARP (D3D12 "Microsoft Basic Render Driver") on Windows. No
adapter at all → the test SKIPs (exit 77), it never fails for missing hardware.

## Build — web

```sh
# Emscripten >= 4.0.10 (bundled emdawnwebgpu port)
emcmake cmake -S spikes/webgpu -B spikes/webgpu/build-web -DCMAKE_BUILD_TYPE=Release
cmake --build spikes/webgpu/build-web
# serve and open in a WebGPU browser (Chrome/Edge); PASS/HASH print to the page + console
python -m http.server 8080 -d spikes/webgpu/build-web
# -> http://localhost:8080/context-spike-webgpu.html
```

## CI

`.github/workflows/ci.yml` job `spike-webgpu` builds the native leg on ubuntu/macos/windows and
runs `ctest` (render check renders on the platforms with a software adapter, skips cleanly
elsewhere); the ubuntu leg re-builds the spike through the integrated `CONTEXT_BUILD_SPIKES=ON`
path as a wiring check. Job `spike-webgpu-web` builds the wasm+JS artifact with emsdk `latest`.
