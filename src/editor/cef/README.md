# `src/editor/cef/` — CEF editor-GUI build substrate (M5-F0a)

The **supply-chain + build substrate** for the M5 observer editor's CEF-embedded GUI
(**L-15** / **L-41** / **R-UI-007** / **R-SEC-009**, issue #150). It is the *foundation* the CEF
editor host (M5-F0b, `src/editor/gui/`) builds on — deliberately **NOT** the host itself: no host
process, no panels, no RPC, no `R-EDIT-001` contract here.

## What it does

1. **Fetches + SHA-256-verifies the pinned CEF prebuilt**, fail-closed (`tools/fetch_cef.py` +
   `tools/cef-prebuilt.json`). CEF is a third-party build lib authenticated by its publisher's TLS +
   SHA-256 pin (the official `minimal` distribution from the Spotify CDN) — the same signed-prebuilt
   carve-out as wgpu-native / rusty_v8 (`docs/signing.md`; **not** the first-party `verify_artifact.py`
   trust root).
2. **Builds `libcef_dll_wrapper`** from the pinned distribution via CEF's own CMake (`find_package(CEF)`).
3. **Boots a CEF browser subprocess headless** (windowless / off-screen, GPU + sandbox disabled),
   loads `about:blank`, and exits 0 — the `cef-substrate-boot` ctest (`src/cef_boot_smoke.cpp`;
   `src/cef_boot_smoke_helper_mac.cpp` is the macOS subprocess helper).

## Building it

Gated behind **`CONTEXT_BUILD_GUI_CEF`** (default **OFF**). Like V8 (`src/runtime/js/`) and wgpu-native
(`src/render/`) it is a **CI-only dependency path**: the CEF prebuilt is MSVC-ABI on Windows /
Clang-ABI elsewhere, so the local Strawberry-GCC Windows `dev` gate cannot fetch/link/boot it. The
default 3-OS build matrix and the local dev gate never enter this directory (it early-returns when the
toggle is OFF). The dedicated **`cef-substrate` CI job** (`.github/workflows/ci.yml`) configures
`-DCONTEXT_BUILD_GUI_CEF=ON` on all 3 desktop OS (Linux under xvfb, Windows on the self-hosted MSVC
runner, macOS bundled), fetching the pinned archive once into a version-keyed cache.

```bash
# On a machine whose toolchain can link the CEF prebuilt (Linux/clang, macOS/clang, Windows/MSVC):
cmake -S src --preset dev -DCONTEXT_BUILD_GUI_CEF=ON
cmake --build --preset dev --target context_cef_boot_smoke   # (run from src/)
ctest --preset dev -R "^cef-substrate-" --output-on-failure   # Linux: wrap in xvfb-run -a
```

The pin (`149.0.6+g0d0eeb6+chromium-149.0.7827.201`) matches the `spikes/cef-compositing` L-41
de-risk, which stays for reference; this promotes the **same** build to a production option. CEF is
BSD-3-Clause (recorded in `tools/license-allowlist.json`).
