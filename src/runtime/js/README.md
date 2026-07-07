# `src/runtime/js/` — in-process V8 JavaScript host

The foundation of the M3 TypeScript scripting tier (issue #76 / **L-61** /
**R-LANG-002/008** / **R-SEC-009** / **R-OBS-005**). This is **task 2a**: a minimal,
polished in-process JS host. The TS toolchain + declarative component-type authoring (R-LANG-010)
are task 2b; the **zero-copy view lifetime & invalidation protocol (R-LANG-009) — `runSystemView`
— lands here** (the detach hard gate), with the (query, executor) tier that consumes it split out
(see "Zero-copy view lifetime & invalidation protocol" below).

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
`VmBuffer::data`. `allocVmBuffer` exposes exactly this primitive; the zero-copy view protocol
(next section) layers per-system ArrayBuffers over the stable store on top.

## Zero-copy view lifetime & invalidation protocol (R-LANG-009) — `runSystemView`

`runSystemView(exec, bindings…)` is the **R-LANG-009 hard gate**, productionized on the real
rusty_v8 host. For each `ViewBinding` (a `{VmBuffer, ViewElement, byteOffset, count}` slice) it
creates a **per-system typed array OVER the VmBuffer's stable backing store** (no copy — the JS
view aliases the exact bytes the host reads/writes), passes the views positionally to the JS
executor, runs it **once**, then **detaches/neuters every view** before returning — even if the
executor throws. A view JS retained past the call is dead afterwards (`byteLength === 0`, element
reads `undefined`, a new view over its buffer throws), while the VmBuffer's bytes survive (the
host stays the sole owner of the store). The detach cost is the §2d gating criterion the spike
measured at ~55 ns (`spikes/js-engine/FINDINGS.md`).

**The ABI technique (why it is CI-only).** rusty_v8's `v8__ArrayBuffer__New__with_backing_store`
shim takes a `__Cr`-ABI `const std::shared_ptr<v8::BackingStore>&`, which a system-STL TU cannot
construct. The host instead hands it a **non-owning** shared_ptr image — a 16-byte
`{ptr, cntrl=nullptr}` POD (`v8_shims.h::BackingStoreSharedPtrImage`, rusty_v8's own
`two_pointers_t` contract): libc++ guards every refcount op with `if (__cntrl_)`, so V8 co-owns
the ArrayBuffer's store but **never frees it** — detach kills the JS view, never the store. The
returned raw pointer is reconstituted into a `v8::Local` by a bit-copy (rusty_v8's `ptr_to_local`).
`Detach` / `IsDetachable` / `v8::XxxArray::New` are STL-free and link directly (no shims). None of
this can link on the local Strawberry-GCC gate — the **3-OS CI legs are authoritative**.

**Deliberately split out (clean follow-up seams on this now-proven primitive):**

- **The (query, executor) system model over the kernel `World`** — query-based archetype
  selection, binding a declared component's column to a VmBuffer, derived TS accessors
  (R-LANG-010 item 1). Builds directly on `runSystemView`.
- **Write-set enforcement** — release hands out only declared-component views (this seam); the
  **debug-build Proxy** that throws on undeclared access is the split half.
- **End-of-system command buffer** — structural changes (add/remove component, entity
  create/destroy) deferred so memory never moves under a live view.
- **WASM-tier analog** — `memory.grow` contract-invalidates the host-mapped linear-memory base;
  the host re-fetches the base at every system entry (R-LANG-009 WASM clause).
- **VM-interior `World` columns** — making the World's authoritative hot columns themselves
  VM-allocated (the R-LANG-008 storage-allocator seam, L-60) so the (query, executor) tier needs
  no column↔VmBuffer sync at all.

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
