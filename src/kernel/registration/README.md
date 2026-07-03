# `src/kernel/registration/` — generated-registration DCE proof (R-KERNEL-003)

This directory de-risks the microkernel's **dead-code-elimination** promise (R-KERNEL-003, an M1
de-risk item on `engine-design/ROADMAP.md` §1 M1): **a package a build does not reference leaves
zero footprint in the final binary.** It validates the "everything is a package, DCE'd builds"
pillar before real packages proliferate.

## The three parts of the mechanism (all required by R-KERNEL-003)

1. **Build-time generated registration TU.** `generate_registration.cmake` runs at build time and
   emits a translation unit defining `register_referenced_packages(Kernel&)`, which calls
   `register_<pkg>(kernel)` for **exactly** the packages the build references — and forward-declares
   only those. The build *enumerates* referenced packages and *generates* the registration code; a
   package not in the list is never named.

2. **Static-initializer self-registration is PROHIBITED.** Registration is always explicit. A
   self-registering object (a namespace-scope object whose constructor registers it) is reachable
   from its own initializer, which structurally defeats `--gc-sections`/DCE — so it is banned in
   shipped builds. The ban is **guarded at runtime**: each probe asserts the module registry is
   empty *before* it calls `register_referenced_packages`. A stray static-init registration would
   make that check fail (`probe_main.cpp` returns exit code 2).

3. **Uniform LTO + linker garbage collection.** `context_dce_flags` (in `CMakeLists.txt`) applies
   `-ffunction-sections -fdata-sections` + `--gc-sections` (GNU/Clang), `-dead_strip` (Apple ld), or
   `/Gy /Gw` + `/OPT:REF /OPT:ICF` (MSVC), plus interprocedural optimization (LTO) where the
   toolchain supports it. The zero-footprint result is delivered primarily by **static-archive link
   semantics** (an archive member is pulled only to resolve a referenced symbol); LTO + section GC
   are the uniform shipped-build pipeline layered on top.

## The measured proof

Two probe executables are built that differ **only** in the generated registration TU each links:

| probe                            | generated TU registers | `alpha` marker in binary |
| -------------------------------- | ---------------------- | ------------------------ |
| `context_dce_probe_referenced`   | `alpha`, `beta`        | **present**              |
| `context_dce_probe_unreferenced` | `beta`                 | **absent** (zero footprint) |

`dce_footprint_check.cmake` (the `kernel-dce-footprint` ctest) greps both linked binaries for each
package's unique marker string and asserts:

- `alpha`'s marker is **absent** from the unreferenced binary — the proof: an unreferenced package
  contributes zero symbol footprint;
- `alpha`'s marker is **present** in the referenced binary — the mechanism does link a referenced
  package (no false-positive elimination);
- `beta` (the control, referenced by both) is **present in both** — the mechanism does not simply
  strip everything.

It also reports the referenced-vs-unreferenced size delta.

## Tests (R-QA-013 — ship with the feature)

Registered under `ctest --preset dev`:

- `kernel-dce-probe-referenced` — happy path: 2 referenced packages register; static-init guard holds.
- `kernel-dce-probe-unreferenced` — edge: only the 1 referenced package registers.
- `kernel-dce-footprint` — the measured symbol/size delta proof (zero footprint for the unreferenced
  package).

The `alpha` / `beta` sources under `packages/` are throwaway **proof fixtures**, not real engine
features; real packages land under `src/packages/`.
