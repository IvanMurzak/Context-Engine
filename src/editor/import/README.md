# src/editor/import — the importer framework

The M2 asset importer framework (issue #60): **R-ASSET-001** (import pipeline), **R-SEC-006/008/010**
(importer isolation), **R-FILE-010** (hybrid derivation cache key), **R-REND-006** (baked-lighting
UV2 hook), **R-QA-011/013** (test corpora + feature-coupled tests).

Package-based importers convert source assets (glTF / PNG / WAV first) into **canonical engine-format
descriptors**. Every import is a **pure, run-deterministic** function of its input, runs **headless**
in EditorKernel, and is **isolated** — the design that makes the shared derivation cache (L-28) sound
and the autonomy envelope safe.

## What lives here

- **The importer contract** (`importer.h`) — `Importer` is stateless + `const`: `import()` is a PURE
  function of `(source bytes, resolved settings, target platform)` — fixed seeds, no threads, no
  clock, no environment, no filesystem — so two runs byte-match and it never throws (a bad source is
  `ok=false` + diagnostics). Artifacts (`DerivedArtifact`) carry a kind + a stable name + the derived
  bytes + their per-kind derived-format version.
- **Three importers** (`importers/`) — each parses the REAL container structure (no third-party
  decode library; deny-by-default deps) into a canonical descriptor:
  - **PNG** (`png_importer.h`) — full chunk walk, **CRC-32 verified**, IHDR geometry + colorspace →
    a texture descriptor. (Texel DEFLATE-decode/encode is the transcode follow-up.)
  - **WAV** (`wav_importer.h`) — RIFF/WAVE `fmt`/`data` parse → an audio descriptor **plus the raw
    PCM payload** artifact. WAV `data` IS raw PCM, so this is a COMPLETE import for the desktop
    `pcm16` target (no deferred decode).
  - **glTF** (`gltf_importer.h`) — GLB container unwrap + glTF-2.0 JSON parse (via the engine's own
    canonical JSON parser) → a mesh descriptor. **Reserves a UV2 (lightmap) channel** whether or not
    the source authored `TEXCOORD_1` (R-REND-006 hook — cheap now, brutal to retrofit).
- **The registry** (`importer_registry.h`) — routes a source path to the importer that claims its
  extension; **collisions are refused, never last-writer-wins**. Package importers join the same
  registry (same contract, same isolation) when the package system lands.
- **The cache key** (`cache_key.h`) — `ImportCacheKey` enumerates **every** input that affects an
  artifact's bytes (R-FILE-010): source bytes + import settings + importer id/version + platform
  profile + **importer build hash** + **CPU ISA** + per-artifact-kind derived-format version + the
  registered schema/migration set hash. `make_cache_key()` fills the build-hash/ISA automatically.
  It EXTENDS the engine's existing FNV-1a content-hash machinery — no new hash family, no restructure
  of the derivation graph's memoization.
- **Isolation** (`sandbox.h`, `isolated_runner.h`, `sandbox_lockdown.cpp`) — R-SEC-006: the shared
  TOCTOU-safe path jail (R-SEC-008, reused from filesync), a **scrubbed environment** (R-SEC-010, C
  locale only — no ambient secret/token), **no ambient network**, and the **input-bytes-only read
  scope with a declared-read-paths escape hatch** (owner ruling, issue #72 — `read_permitted` grants
  only `input_path ∪ declared_read_paths`, all ⊆ jail; the writable set is the own cache-output key).
  Two runners share the policy: `run_isolated()` (the in-process reference) and **`run_subprocess()`**
  — on Linux + macOS it fork()s an **unprivileged child locked down by the OS primitive** (a
  hand-written seccomp-bpf filter on Linux / a deny-default Seatbelt profile via `sandbox_init` on
  macOS, `apply_importer_sandbox`, no third-party dep on either), runs the pure importer there, and
  pipes the `ImportResult` back (`encode/decode_import_result`); `os_sandbox_support()` reports the
  primitive is **enforced** on Linux + macOS and honestly `enforced=false` (in-process fallback) on
  Windows. `check_deterministic()` is the double-run byte-compare gate.
- **The per-platform transcode table** (`platform_profile.h`) — the v1 platform set + the data-driven
  `(kind × platform) → format` table. The platform profile is a cache-key component, so per-platform
  variants coexist as separate entries (platform switches instant after first import).
- **The per-platform transcode NODE** (`transcode.h`, task a03 — R-BUILD-003) — the byte-level encoder
  over that table: `transcode_variant(artifact, platform)` turns one derived artifact into the target's
  VARIANT payload (a self-describing `CTXV` container), which the pack writer selects per target
  (`src/editor/pack/`). PURE + deterministic like an import, so the double-run gate holds with transcode
  nodes in the graph. It RIDES the existing key — `transcode_cache_key()` reuses the R-FILE-010 platform
  component rather than forking a second cache. Each variant records the table's `target_format` AND the
  `applied_encoding` v1 produced, so a trailing encoder is visible in the data (see § Explicitly deferred).

## The determinism gate + fuzz corpus

- `import-test_determinism` — the **R-ASSET-001 CI double-run byte-compare gate**: imports each
  fixture + every routable corpus seed twice and asserts byte-equality (and proves it has teeth by
  catching a deliberately-flaky importer). The `import.non_deterministic` catalog code is already
  reserved for a future runtime consumer that surfaces a gate failure as a protocol error — v1
  asserts byte-equality directly in this test, so nothing emits the code yet (mirroring the
  `import.cache_corrupt` reservation for the on-disk cache STORE follow-up).
- `import-test_fuzz_corpus` — the **R-QA-011 / R-SEC-006 corpus replay**: every committed seed
  (valid + minimized malformed) under `tests/corpora/` is replayed through the jailed runner,
  asserting no crash, a graceful result, determinism, and the isolation audit. This is corpus
  regression, **never open-ended fuzz time per PR**. Regenerate the corpus with
  `python tests/corpora/generate_corpus.py` (deterministic).

## Explicitly deferred (tracked follow-ups — NOT silently assumed)

The owner bar is "very well polished, not just drafted" — the deferrals below are **format/contract
hooks staked out now** with the heavy work tracked, never silent stubs:

1. **Byte-level transcode — LANDED (task a03, R-BUILD-003).** `transcode.h` turns a derived artifact
   into its per-platform VARIANT payload (a self-describing `CTXV` container), which the pack writer
   selects per target (`src/editor/pack/`, `PackWriteOptions::target_platform`). As predicted, it
   needed NO cache-key change: `transcode_cache_key()` RIDES the existing platform component
   (`transcode_variant`'s target comes from the same `transcode_target_for()` table). Per kind, stated
   honestly — `TranscodedVariant` records BOTH the table's `target_format` and the `applied_encoding`
   v1 actually produced, so a trailing encoder is visible in the data, never a silent stub:
   - **audio — REAL + complete.** Desktop `pcm16` stores the verbatim 16-bit PCM (WAV `data` IS raw
     PCM); the memory-constrained Web target applies **G.711 µ-law companding** (`mulaw8`) — a real,
     standard, dependency-free 2:1 compression. Residual: the perceptual **Vorbis** encoder the table
     names for Web (`target_format` "vorbis" vs `applied_encoding` "mulaw8").
   - **texture — REAL block geometry, texels residual.** The variant is the true BCn/ASTC block-tiled
     container: block footprint (4×4 BCn vs 8×8 ASTC), the mip pyramid, and each level's encoded byte
     length, all computed from the real descriptor — so desktop and Apple/Web variants are genuinely
     different payloads. The perceptual **texel encoder** (BC7/ASTC compression of actual pixels) is
     blocked on the PNG **texel DEFLATE-decode**, which this module still defers: the importer emits
     no texels to compress yet (`applied_encoding` "block_plan"). Landing the decode fills the payload
     behind the same container + cache key.
   - **mesh — plan only.** `meshopt` on every v1 target (platform-invariant, so no per-platform pack
     variant is needed); the vertex-stream quantizer is the residual (`applied_encoding` "meshopt_plan").
2. **Per-OS subprocess sandbox lockdown — LANDED on Linux + macOS (issue #72).** `run_subprocess()`
   now runs the pure importer in a fork()ed, OS-locked unprivileged child on Linux (a **seccomp-bpf**
   filter) AND on macOS (a **deny-default Seatbelt profile** applied post-fork via `sandbox_init` — the
   same SBPL/Seatbelt mechanism as the `sandbox-exec` CLI, in-process; no third-party dependency) —
   `os_sandbox_support()` reports `enforced=true` on both. Windows AppContainer / restricted Job Object
   remains the one **tracked de-risk item** (`enforced=false`, in-process fallback). Residual
   follow-ups, staged honestly: (a) the reference runner forks **without exec**, which is safe in the
   single-threaded CLI/reference context but not in a multi-threaded daemon, and the child inherits the
   parent's fd table (the primitive allows read/write on already-open fds, blocking only fresh `open*`)
   — the daemon integration hardens BOTH by moving to a fork+exec importer-host (O_CLOEXEC scrubs the fd
   table) or a pre-forked zygote (the sandbox primitive + read-scope contract are unchanged); (b) the
   Linux seccomp filter gates on the x86_64 ABI (the Linux-first server target) — an ARM64 Linux runner
   is a follow-up. Importers stayed pure, so the swap needed **no importer change**.
3. **Importer build hash.** v1 derives it from the framework epoch + the toolchain stamp (re-keys on
   a toolchain change — the cross-machine determinism scope R-FILE-010 defers). A real per-importer
   compiled-object hash lands with the native build pipeline.
4. **On-disk cache STORE + corrupt-entry self-heal.** The exhaustive R-FILE-010 cache KEY (with its
   sensitivity-matrix test) is complete, but v1 does not yet persist a write-once/self-verifying cache
   STORE — a call re-derives rather than reading a stored artifact back. The store's write-once +
   self-verify + re-derive-on-mismatch behavior, and its corrupt-entry runtime rejection test, are the
   follow-up; the `import.cache_corrupt` catalog code is already reserved for it.
5. **Async progress events.** Imports are synchronous in v1 — there is no async consumer yet (no CLI
   import verb / daemon progress channel this milestone), so `import()` returns a whole `ImportResult`
   rather than streaming. Progress-event emission (and its R-QA-013 test) is deferred until that
   consumer lands; the importer contract stays pure, so it slots in with no importer change.

## Layering

Depends only on `context_serializer` (canonical JSON parse/emit + the content-hash family) and
`context_filesync` (raw-byte content hash + the shared R-SEC-008 path jail), both PRIVATE — no public
`import` header exposes their types. Emits diagnostics through the shared error catalog
(`contract/error_catalog.cpp`, the `import.*` cluster). No third-party dependency (deny-by-default).
