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

## Related: `m1-exit-3` instrumented-boot timeout flake

The `m1-exit-3-crash-recovery` kill-9 test (`src/tests/integration/`) flaked ~2/3 on the same
`sanitize` leg under concurrent runner load — not a defect, but its daemon-boot discovery-hint
wait (`itest::wait_for_instance`, plain 15s) is too tight for an instrumented daemon boot. Fixed
in issue #201 by scaling that wait under a sanitizer build (`itest::scaled_timeout_ms`,
`integration_test.h`); the plain dev/CI legs keep the tight timeout unchanged.
