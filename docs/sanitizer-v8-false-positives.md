# Sanitizer false positives from the uninstrumented rusty_v8 prebuilt

Root-cause investigation for the `sanitize (ASan+UBSan, ubuntu)` CI leg's UBSan `vptr` +
LSan demangler-scratch pattern (issue #201; surfaced by the M6 X1 GC work, PR #189). This
note is the authoritative determination behind the narrow suppressions wired in
`src/runtime/ts/lsan-suppressions.txt`, `src/runtime/js/tsan-suppressions.txt`, and the
`CONTEXT_SANITIZE` / `CONTEXT_TSAN` blocks in `src/cli`, `src/runtime/ts`,
`src/tests/integration`, and `src/editor/merge` `CMakeLists.txt`.

## Verdict

**Tooling false positive, not a Context defect.** The UBSan `vptr` diagnostics fire on
*valid* libstdc++ stream objects; the LSan "leaks" are allocated entirely inside the
sanitizer's own demangler. Neither originates in `namespace context::` code. A **narrow,
sanitizer-internal-only** suppression is therefore the correct minimal fix (and is what the
repo ships) — it can never mask a real Context bug.

## Trigger

The M6 X1 GC-profiler work (PR #189, issue #188) added a `context_cli → context_js`
linkage (`context profile gc`). On the V8-capable CI legs that bakes the SHA-pinned
**denoland/rusty_v8** static archive — the DEFAULT `release` build, **not**
sanitizer-instrumented, bundling Chromium's own hidden-visibility libc++
(`use_custom_libcxx`, the `__Cr` ABI) — into every cli-family binary and the `context`
binary the e2e tests spawn. The local Windows GCC `dev` gate links the tiny `context_js`
stub instead, so this pattern is invisible locally and is a **CI-only** phenomenon.

## Mechanism (two coupled symptoms)

1. **UBSan `vptr` false positive (benign noise).** The uninstrumented archive contributes a
   *second*, hidden-visibility copy of the C++ runtime `type_info`. UBSan's `-fsanitize=vptr`
   check verifies a polymorphic object's vptr by `type_info` **identity** (pointer compare),
   which now fails across the duplicate — so it flags perfectly valid libstdc++ streams.
   Observed on the last green run (job `86715274494`, run `29217177110`):

   ```
   src/editor/filesync/src/native_file_store.cpp:333:10: runtime error: cast to virtual base
     of address 0x… which does not point to an object of type 'std::basic_ifstream<char>'
       vptr for 'std::basic_ifstream<char, std::char_traits<char>>'
   src/editor/filesync/src/reconcile_index.cpp:35:13: runtime error: member call on address …
     which does not point to an object of type 'std::basic_ostream<char>'
       vptr for 'std::__cxx11::basic_ostringstream<char, std::char_traits<char>, …>'
   ```

   The diagnosed types are the **system libstdc++** streams (`std::__cxx11::` ABI; the source
   locations are `/usr/lib/gcc/x86_64-linux-gnu/14/…/bits/streambuf_iterator.h`) — genuinely
   valid objects. The preset compiles with `-fsanitize=address,undefined` and **no**
   `-fno-sanitize-recover`, so UBSan **recovers**: these lines print but never abort a test.
   They are pure stderr noise (the "masks real sanitizer signal" concern) — they are not what
   fails a test.

   > 🚨 **Do NOT re-diagnose this signature. It has now been misfiled twice.** Because the
   > `sanitize` leg runs `ctest --verbose`, these recovered `vptr` lines are printed by tests that
   > **PASS**, so they show up in the log of any red run and read like the cause. Issue **#335**
   > was filed on exactly the two citations quoted above — `native_file_store.cpp:333` and
   > `reconcile_index.cpp:35` — hypothesising "a `static_cast` downcast of an istream to
   > `std::ifstream`". There is no cast at either site (`std::ifstream in(...); if (!in)` /
   > `in.bad()`; `std::ostringstream out; out << …`), and on the run that filed it those lines came
   > from tests **84 and 85, both green**, while the job's single failure was a different test
   > entirely (`406 - m6-exit-2-gc-budget`, see § Related below). **Triage rule**: the verdict is
   > `The following tests FAILED:` at the tail of the ctest output — never the first `runtime
   > error:` line in the log. UBSan's own `note:` also refutes the report on sight (it names the
   > object's real dynamic type, e.g. expected `'std::basic_ifstream<char>'` vs note `object is of
   > type 'std::basic_ifstream<char, std::char_traits<char>>'` — the same type).

2. **LSan demangler-scratch leak (what actually fails a test).** Symbolizing each `vptr`
   diagnostic demangles the offending type name: `__sanitizer::Symbolizer::PlatformDemangle`
   → `DemangleCXXABI` → `__cxa_demangle` → LLVM's `itanium_demangle::`. The demangler's
   internal scratch buffers are still reachable at process exit, where LSan (bundled with
   ASan) reports them as leaks whose allocation stack unwinds *into the uninstrumented
   archive*. Same run, the LSan report across ~20 cli-family tests:

   ```
   Suppressions used:
        24      23880 itanium_demangle::
   ```

   Every leaked byte is allocated INSIDE the sanitizer's demangler — never in `context::`
   code. A non-empty LSan report is a non-zero exit, i.e. a failed test; unsuppressed this is
   the ~23/265 failures (~9% of the suite).

## The fix the repo ships (narrow, cannot mask real signal)

`LSAN_OPTIONS=suppressions=src/runtime/ts/lsan-suppressions.txt` is wired onto every V8-linking
ctest (and spawned `context` children inherit the env). Its four rules match **only**
sanitizer/demangler-internal frames:

```
leak:__sanitizer::Symbolizer::PlatformDemangle
leak:DemangleCXXABI
leak:__cxa_demangle
leak:itanium_demangle::
```

A real Context leak carries a `context::` frame in its allocation stack and matches **none**
of these, so it is still reported and would be **fixed, not suppressed**. The
`Suppressions used: N itanium_demangle::` lines in every run confirm the rules only ever match
the demangler scratch. The `-fno-sanitize=vptr` carve-out used for `context_ts`
(`src/runtime/ts`) is **not** viable for the cli family: the `vptr` diagnostics fire in TUs of
libraries (`filesync`, `merge`, `cli`) shared with non-V8 binaries, whose vptr coverage must
stay — so for the cli family this LSan suppression is the operative fix and the recovered vptr
lines remain as known-benign noise.

## Alternatives considered

| Option | Why not (yet) |
| --- | --- |
| `-fno-sanitize=vptr` globally on the `sanitize` preset | Discards ALL vptr coverage repo-wide — that *is* masking real signal, the opposite of the goal. |
| `-fno-sanitize=vptr` on the `filesync`/`merge`/`cli` TUs | Those TUs also link into NON-V8 binaries where the vptr check is valid and wanted; a per-TU flag would blind those too. |
| **At-source `UBSAN_OPTIONS=suppressions=…` with `vptr_check:std::basic_*`** | The *ideal* real fix: suppressing the vptr FP at the root removes the noise AND the demangler leak, so no LSan net is needed. **Deferred** — the `sanitize` preset is non-Windows-only and cannot be built on the local executor host (design lock L-38), so this is verifiable ONLY by a live CI run; and a sanitizer that rejects a suppressions file (unknown type / bad path) FATALs at startup and would redden the whole currently-green leg. Recommended as an **owner-gated follow-up** validated by a deliberate sanitize-CI run, not gambled into a green gate. Recipe below. |

### Deferred at-source recipe (owner-gated)

Add alongside the existing `LSAN_OPTIONS` on the same cli tests (keep the LSan net as
belt-and-suspenders on the first validating run):

```
UBSAN_OPTIONS=suppressions=<abs>/ubsan-vptr-suppressions.txt
```

```
# ubsan-vptr-suppressions.txt — suppress the rusty_v8-duplicate-typeinfo vptr FP at source.
vptr_check:std::basic_ios
vptr_check:std::basic_istream
vptr_check:std::basic_ostream
vptr_check:std::basic_ifstream
vptr_check:std::basic_ofstream
vptr_check:std::basic_filebuf
vptr_check:std::basic_streambuf
vptr_check:std::__cxx11::basic_ostringstream
vptr_check:std::__cxx11::basic_istringstream
vptr_check:std::__cxx11::basic_stringbuf
```

Validate on a real `sanitize (ASan+UBSan, ubuntu)` run before removing the LSan net; if the
noise disappears and no new failures appear, the LSan demangler suppression becomes redundant.

## Related: `m6-exit-2` ASan+UBSan GC-budget widen gap (issue #335)

The sibling of the `m1-exit-3` story below, on the *budget* axis rather than the *timeout* axis, and
the ACTUAL cause of the red that issue #335 misattributed to the `vptr` noise above.

`m6-exit-2-gc-budget` asserts a REAL wall-clock ceiling — `kBudgetMs = 4.167 ms`, the R-SIM-008
quarter-timestep — which is only honestly measurable on an UNINSTRUMENTED build. When the gate
landed it widened that ceiling 100x for TSan (`CONTEXT_TSAN_BUILD`) but left ASan+UBSan enforcing the
real budget, because the ASan+UBSan leg then measured `maxPauseMs=0.991` and looked to have 4x
headroom. It does not. Measured on run `29987903124` — ONE commit, bit-identical workload
(`ticks=600 pauses=10 inWindow=10 dropped=0`):

| leg | maxPauseMs | enforced | result |
| --- | --- | --- | --- |
| `build (ubuntu / macos / windows)` — uninstrumented | (real budget holds) | 4.167 | green |
| `sanitize (TSan, ubuntu)` | 114.592 | 416.667 (100x) | green |
| `sanitize (ASan+UBSan, ubuntu)` | **4.473** | **4.167 (1x)** | **RED** |

Across the thirteen consecutive ASan+UBSan runs of that same workload spanning jobs `89084381975` …
`89143913144`, `maxPauseMs` ranged **1.056 … 4.473 ms** — a 4.2x load-driven spread whose tail
crosses the ceiling.
The gate's margin was under 1x of its own measurement noise, so it reds intermittently with no engine
regression; that intermittency is what made it read as a flake. Fixed by widening ASan+UBSan 10x
(`CONTEXT_ASAN_BUILD`, 9.3x margin over the worst observation, still an order of magnitude tighter
than TSan's) and by a compile-time guard in `test_m6exit2_gc_budget.cpp` that asks the COMPILER
whether a sanitizer is active and fails the build if the CMake wiring plumbed no widen for it — the
defect was the missing wiring, not the constant, so the guard is on the wiring. The guard asserts
BOTH directions: a plumbed widen define with no compiler-reported sanitizer is an `#error` too, so a
detection that ever goes blind reds the leg at build time instead of silently passing vacuously and
leaving the guard inert.

**Generalises**: any real-time budget assertion added to this repo must be widened for BOTH sanitizer
presets in the same PR (`conventions.md` § "Real-time / wall-clock budget assertions must be
TSan-aware"), and the uninstrumented `build` legs remain the only place a real-time property is
actually verified.

## Related: `m1-exit-3` instrumented-boot timeout flake

The `m1-exit-3-crash-recovery` kill-9 test (`src/tests/integration/`) flaked ~2/3 on the same
`sanitize` leg under concurrent runner load — not a defect, but its daemon-boot discovery-hint
wait (`itest::wait_for_instance`, plain 15s) is too tight for an instrumented daemon boot. Fixed
in issue #201 by scaling that wait under a sanitizer build (`itest::scaled_timeout_ms`,
`integration_test.h`); the plain dev/CI legs keep the tight timeout unchanged.

A **second, distinct** timing flake in the same test hit a NON-sanitizer leg: the post-recovery
daemon process-EXIT waits (`ctest_proc::wait_for(daemon…, 15000)` — the killed daemon's reap and
daemon2's clean-shutdown exit) were HARD, UNSCALED 15s ceilings, so under concurrent runner load
daemon2 overshot 15s and `CHECK failed: daemon2_code == 0` fired (`shader-crosscompile
(macos-latest)`, run 29201369515: attempt 1 RED, attempt 2 GREEN, same commit). Fixed by raising
those two ceilings to a load-safe 60s (`kDaemonExitWaitMs`, routed through `itest::scaled_timeout_ms`
so the sanitizer legs keep their headroom too) — far below the 600s ctest TIMEOUT, so a genuinely
non-recovering daemon still fails (a crash returns non-zero at once; a true hang still trips the
ceiling). This is the process-EXIT sibling of the issue #201 daemon-BOOT fix above.
