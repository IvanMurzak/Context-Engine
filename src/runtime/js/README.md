# `src/runtime/js/` — in-process V8 JavaScript host

The foundation of the M3 TypeScript scripting tier (issue #76 / **L-61** /
**R-LANG-002/008** / **R-SEC-009** / **R-OBS-005**). This is **task 2a**: a minimal,
polished in-process JS host — nothing more. The TS toolchain + declarative component-type
authoring (R-LANG-010) are task 2b; the full zero-copy view protocol (R-LANG-009) is task 3.

## What this package provides

- **`JsEngine`** (`include/context/runtime/js/js_engine.h`) — the backend-agnostic,
  STL-only host interface. This is the **from-source migration seam** (see below).
- **`createV8Engine`** (`include/context/runtime/js/js_host.h`) — the V8 backend factory.
- Isolate + Context lifecycle, trivial JS **eval** from C++, a **host↔JS boundary call each
  way** (`getFunction`/`callFunction` and `bindHostFunction`), the **shape-B VM-allocated
  (sandbox-interior) backing-store seam** (`allocVmBuffer`/`freeVmBuffer`), and a **stubbed
  CDP inspector seam** (R-OBS-005).

## Supply chain — SHA-pinned rusty_v8 prebuilt (R-SEC-009)

V8 is acquired as a **SHA-pinned third-party prebuilt** from
[`denoland/rusty_v8`](https://github.com/denoland/rusty_v8) (static libs) plus the
version-coherent [`v8` crate](https://crates.io/crates/v8) (the C++ headers rusty_v8 does not
ship). This is the **wgpu-native / CEF precedent** codified in `docs/signing.md`: third-party
build libraries are authenticated by **their own publisher** via **TLS + SHA-pin +
verify-before-use** and are **out of scope for the engine's first-party R-SEC-009 trust root**
— so they are deliberately **not** routed through `tools/verify_artifact.py`.

- Pins: `tools/v8-prebuilt.json` (the single source of truth — rusty_v8 **v149.4.0** → V8
  **14.9.207.2**; per-triple static-lib SHA-256 + the header crate SHA-256).
- Fetch/verify/decompress: `tools/fetch_v8.py` (fail-closed; `--source` offline escape hatch;
  covered by `tools/tests/test_fetch_v8.py`).
- **Version-coherence** (the lib and headers come from the same V8) is the critical
  correctness invariant — asserted at runtime by `fetch_v8.py` and statically by the tests.
- Wiring: `CMakeLists.txt` reads the manifest, invokes `fetch_v8.py` at configure time, and
  fails the build closed if verification fails.

## The hybrid host (why it is built this way)

Every published rusty_v8 prebuilt is `use_custom_libcxx=true` — Chromium's bundled libc++ in
the `__Cr` ABI namespace. So any `v8::` C++ API whose signature embeds a by-value STL type
(mangled `std::__Cr`) **cannot be linked** by a system-STL embedder (libstdc++ / MSVC-STL /
system libc++) on any CI leg. The host therefore uses a **hybrid**:

- The **STL-free majority** of the surface (Isolate, HandleScope, Context, Script compile/run,
  String, FunctionTemplate — host↔JS both ways) uses the **real `v8::` C++ API** directly.
- The **two STL-crossing ops** (platform boot; the shape-B backing store) go through
  rusty_v8's own **`extern "C"` `v8__*` shims** (`src/v8_shims.h`) — defined in the *same*
  archive, unmangled, and ABI-namespace-immune. Signatures are pinned verbatim to
  `denoland/rusty_v8@v149.4.0`'s `src/binding.cc`.

rusty_v8's bundled libc++ is statically internal to the archive (the `__Cr` namespace exists
precisely to coexist with the host STL), so there is no host/runtime STL conflict.

## Shape-B storage (R-LANG-008, amended 2026-07-05)

Stock V8 (`V8_ENABLE_SANDBOX`, on in every prebuilt) **fatally aborts** on host-owned external
backing stores, so "engine-owned" means **lifetime/authority, not allocator identity**: the VM
allocates a sandbox-interior backing store and the host reads/writes it through
`VmBuffer::data`. `allocVmBuffer` exposes exactly this primitive; task 3 layers the zero-copy
view protocol (per-system ArrayBuffers attached over the stable store + the R-LANG-009 detach)
on top. Task 2a proves only that a VM-allocated buffer is **host read/write visible**.

## Local vs CI (build gate)

The rusty_v8 prebuilt is an **MSVC/Clang-ABI static lib**, so it is **CI-only for its
dependency path**: the local Ninja + Strawberry-GCC Windows `dev` gate cannot link it and
builds a **stub** backend (`createV8Engine` returns `nullptr`; the ctest asserts that). The
**3-OS CI `build` legs** (Linux-x64 clang / macOS-ARM64 clang / Win-x64 MSVC) are the
**authoritative gate** (profile `setup.md`/`test.md` carve-out).

## Footprint (measured — spike `spikes/js-engine/FINDINGS.md`, 2026-07-02)

- **Binary payload:** ~**48 MB** added to a shipped host (V8 + ICU data + support libs). Note
  rusty_v8 builds V8 with `v8_use_external_startup_data=false`, so the **snapshot + ICU data
  are embedded in the archive** — there are **no external `icudtl.dat` / `snapshot_blob.bin`
  files** to stage next to the executable (unlike the spike's NuGet monolith). The payload
  moves from side-car DLLs into the statically-linked binary.
- **Startup → first eval:** ~**6.9 ms** (process-cold; irrelevant for an editor daemon /
  desktop game).
- **Memory floor after init:** ~**14 MB** private bytes.

These costs were accepted with eyes open in the §2d engine selection (V8 won on interpreter
throughput, the R-LANG-009 detach gate, the R-OBS-005 debugger story, and GC pacing).

## From-source migration seam (tracking note — future task, do NOT do now)

`JsEngine` is the seam. Migrating to an **owned-from-source V8** (`use_custom_libcxx=false`, a
V8 port in the engine's vcpkg registry) is a **separate pre-1.0 / marketplace hardening task**:
it would restore direct use of the STL-typed `v8::` API (retiring the `v8_shims.h` hybrid), let
the engine sign the binary with its own R-SEC-009 production key, and remove the third-party
prebuilt dependency. When taken, it is a **clean backend swap behind `JsEngine`** — no consumer
of this interface changes. It is explicitly out of scope for task 2a (owner ruling, issue #76).
