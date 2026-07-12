# `context_audio` — audio system package (M6 P6, R-SYS-006 / L-46)

The engine's audio system on the **miniaudio** backbone (L-46). Unlike every sibling M6 package (which
has a deterministic sim half AND a presentation half), audio is the **FIRST fully-observer package**:
it is **entirely a presentation observer** (R-SIM-001), OFF the deterministic sim path.

- It **reads** sim state from a `const kernel::World` (entity positions for spatialization, triggers)
  — so it structurally **cannot write** sim state.
- It holds its own **float** mixer state (`audio_engine.cpp`), never in the World, never a registered
  sim component.
- Its miniaudio **audio thread** and float mixing fold into **no** state hash and taint **no**
  deterministic build. `tests/test_observer_hash.cpp` PROVES that arbitrary audio activity — including
  the running audio thread — leaves the L-54 hierarchical state hash **byte-identical** and registers
  no sim component (the m6-exit-5 seam invariant).

Because audio has no sim state, there is **no** `determinism-*-scene` ctest for it and it is **not** on
the `deterministic` CI job's `--target` list.

It is a package, not kernel core (L-60 microkernel invariant): it composes on the kernel `World` as a
read-only observer; the kernel never links back.

## miniaudio (the L-46 backbone)

miniaudio is a single-header C audio library **vendored** at `third_party/miniaudio/miniaudio.h`
(pinned **v0.11.22**, dual-licensed Unlicense OR MIT-0 — we elect **MIT-0**, recorded in
`tools/license-allowlist.json`). It is compiled as its own **warnings-suppressed** static lib
(`context_audio_miniaudio`) with only the **NULL device backend** enabled
(`MA_ENABLE_ONLY_SPECIFIC_BACKENDS` + `MA_ENABLE_NULL`) — CI runners have no audio hardware and the
tests never open a real device. The high-level engine / decoders / encoders are disabled; the package
does its own low-level float mixing (distance attenuation + equal-power stereo pan + per-bus gain) in
the device callback, on miniaudio's own low-latency audio thread. The mix advances voice envelopes in
wall-clock time (frames / sampleRate), so it is **frame-rate independent** of the sim tick (R-SYS-006).
miniaudio is a PIMPL implementation detail — it never leaks into the package's public headers.

> The current mixer takes a brief lock on the control path and a `try_lock` in the audio callback (a
> contended buffer is dropped to silence rather than blocking the audio thread). A lock-free ring
> buffer is a production-hardening follow-up; the null-backend foundation prioritizes correctness.

## Authored content kinds

Two authored kinds are registered in `schema::engine_schemas()` (SCHEMA in `src/editor/schema/`) with
their referential semantics in `src/editor/kinds/`:

- **`ctx:audio-bus`** — a mixing-bus graph (named buses with a linear gain + optional parent). Semantic
  rules (`kinds/audio_bus.h`): unique bus ids, every parent resolves, the parent graph is acyclic.
- **`ctx:audio-event`** — a sound event (a DCC-imported clip routed to a bus, with an optional 3D
  spatialization block). Semantic rule (`kinds/audio_event.h`): a consistent attenuation range
  (`maxDistance > minDistance`, both non-negative).

Each ships a `samples/**/*.<kind>.json` corpus entry (`samples/audio-buses/`, `samples/audio-events/`)
carrying `"$schema": "<id>"`, satisfying the blocking samples-corpus gate.

## Layout

```
include/context/packages/audio/
  errors.h            the audio.* fail-closed error-code strings (contract-catalog source of truth)
  audio_engine.h      AudioEngine — the miniaudio null-backend device + float mixer (PIMPL)
  observer.h          AudioObserver — the presentation observer over the const World
src/
  audio_engine.cpp    the device + mixer (the package's float TU — off the hash)
  observer.cpp        the observer (reads session::Position read-only)
  miniaudio_impl.cpp  the vendored miniaudio implementation TU (warnings-suppressed sub-lib)
third_party/miniaudio/
  miniaudio.h         vendored single header (pinned v0.11.22, MIT-0)
tests/
  test_engine.cpp         device lifecycle + mixer (bus gain, attenuation, R-SYS-006 frame-rate independence)
  test_observer.cpp       observer reads positions read-only, invalid_entity refusal, world untouched
  test_observer_hash.cpp  the R-SIM-001 / m6-exit-5 off-the-hash presentation-observer invariant
  test_errors.cpp         the audio.* fail-closed codes, pinned to the catalog
```

## Tests

`ctest --preset dev -R "^audio-"` runs the unit suites. Audio has no determinism gate (no sim state).
All ship in the same PR as the package (R-QA-013).
