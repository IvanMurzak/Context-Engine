# Why every CEF smoke isolates the OSCrypt keychain (issue #437)

A CEF smoke on macOS used to do all of its work, print its whole success verdict, and then **never
exit**. `ctest` reported `***Timeout`; the scenario had in fact finished in ~1.6 s. This note records
the mechanism, the measurement, and the one line each CEF executable now carries — because the failure
mode is invisible to every gate we have, it was misdiagnosed twice, and the next task in this area
would otherwise re-derive it.

## The mechanism

Chromium's `OSCrypt` derives the profile-encryption key from a **machine-global** generic-password
keychain item named `"<product> Safe Storage"` — for a CEF app, `"Chromium Safe Storage"`, shared by
every Chromium-branded app on the box. It reads it on a `base::ThreadPool` task posted
**`BLOCK_SHUTDOWN`**.

macOS binds that item's ACL to the **code signature of the executable that created it**. For a locally
built, ad-hoc-signed binary that means its **cdhash** — which changes on every rebuild. So any other
executable, *including the same target after a rebuild*, is off the ACL, and `securityd` falls through
to the item's `KeychainPromptAclSubject`: it spawns `SecurityAgent` to ask a human. Until someone
answers, `SecItemCopyMatching` blocks inside `SecurityServer::ClientSession::decrypt`.

Nobody answers on an unattended host. The `BLOCK_SHUTDOWN` task therefore never completes, and
`CefShutdown()` — which waits on the ThreadPool shutdown event — never returns. The smoke has already
printed its verdict by then, which is why the symptom reads as "passed, then hung in teardown".

`securityd`'s own log states it outright:

```
[com.apple.securityd:acleval] ObjectAcl REJECTS access using ACL:
  ... SUBJECT[<CodeSignatureAclSubject[
        path:   .../worktrees/macos-cef-probe/.../editor/gui/host/Release/context_gui_host.app
        requirement: cdhash H"404d1922095ca8b935abb4eec3787bfd4a5c9972"]>]
      [<KeychainPromptAclSubject(flags: 0x0, desc:Chromium Safe Storage)>] ...
```

## Why it looked OS-version-specific, and was not

The hang was reported as a macOS 26 (Tahoe) regression against Chromium 149. It is not: the
discriminator is **whether the keychain item already exists and who created it**.

| time (2026-07-26, one host) | result |
|---|---|
| ~05:15 | `editor-cef-smoke-boot` **PASSED 2/2** |
| **05:19:22** | the `"Chromium Safe Storage"` item is **CREATED** (`cdat`), by a `context_gui_host` build in another worktree |
| ~09:20 onwards | every CEF executable **HUNG**, from every worktree, on every commit |

Creating the item is implicitly authorized for the creating process, so the first run ever on a clean
keychain passes and *installs the trap* for everything after it. That is also the CI shape: on
`macos-latest`, `editor-cef-smoke-boot` ran first and passed (3.09 s) while `editor-cef-smoke-shell`
and `-shell-restore` — different executables, same job, same keychain — timed out. `cef-substrate` is a
separate job on a separate fresh runner, so its single executable always creates the item and is always
green.

Two consequences worth keeping:

* **A green CI leg proves nothing here.** The hang is state-dependent, so when a hosted image's
  keychain state changes it arrives as an *intermittent* red across three jobs.
* **A hung test asserts nothing**, so no runtime assertion can catch the omission. That is why the
  isolation is enforced from the SOURCES (below).

## The fix

Chromium's own test switch, `--use-mock-keychain`, replaces the real Apple keychain with an in-process
fake, so no machine-global state is touched at all. It must be a **command-line switch**: Chromium's
Chrome-layer main reads it to install the fake keychain before the first `OSCrypt` use, and there is no
CEF API surface for it.

Two shapes, because there are two ways a CEF app here reaches Chromium's command line:

* **The Shell** exposes `CefShellOptions::use_mock_keychain` (default **false** — a shipped editor keeps
  the real OS key store). `cef_shell.cpp` latches it before `CefInitialize` and appends the switch in
  `OnBeforeCommandLineProcessing`. Every smoke under `src/editor/shell/cef/src/` sets it. They cannot
  do it any other way: the Shell's `initialize()` builds `CefMainArgs(0, nullptr)` on POSIX, so a
  switch on the executable's own argv never reaches Chromium — an argv-level A/B on those binaries
  changes nothing, which briefly read as "the fix does not work".
* **Standalone CEF apps** that own their own `CefApp` (`src/editor/cef/src/cef_boot_smoke.cpp`,
  `src/editor/gui/host/src/editor_host.cpp`) append the switch directly.

This is the **second** piece of shared machine state a CEF smoke must isolate; `CefShellOptions::
cache_root`'s per-PID temp dir (the Chromium process singleton) is the first. Same shape, same reason:
a test may not depend on machine-global state another process — or another build of itself — also owns.

### Measured, on one binary, one minute apart

| condition | `cef-substrate-boot` |
|---|---|
| no switch | **5/5 HUNG** at a 60 s budget, each after printing its full success line |
| `--use-mock-keychain` | **5/5 PASS**, ~1 s each |

And after the in-code fix, the two smokes that e12c-1 had shipped `DISABLED TRUE`:

| ctest | rate |
|---|---|
| `editor-cef-smoke-shell` | **PASS 5/5** (2.68 s cold, 0.60 s warm) |
| `editor-cef-smoke-shell-restore` | **PASS 5/5** (3.48 s cold, ~1.30 s warm) |

`-shell-restore` phase 1 prints `cef::shutdown() (CefShutdown) returned` and its
`cef_shutdown_returned` check passes — so the **CE #319 teardown-ordering invariant is now proven on
macOS**, where it had been skipped. Nothing about teardown was bounded, watchdogged or skipped to get
there.

## What enforces it

* **`tools/check_cef_keychain_isolation.py`**, registered as the `editor-shell-cef-keychain` ctest (the
  non-gate `editor-shell-*` family, so it auto-runs on all three CI `build` legs and locally, and needs
  no CEF build). Both its subject sets are **predicates**, not lists — "constructs a `CefShellOptions`
  under `src/editor/shell/cef/src/`" and "defines `OnBeforeCommandLineProcessing` under
  `src/editor/`" — because a planting round caught a filename-keyed and a hardcoded-list version each
  letting a new CEF source through GREEN. It also has an anti-vacuity half: the option must still be
  declared, latched and appended, or deleting it would make the gate pass on a tree that isolates
  nothing. Its pytest is `tools/tests/test_check_cef_keychain_isolation.py`.
* **`tools/measure_cef_smoke_rate.py`** re-measures the rate. It classifies each run
  `PASS` / `FAIL(rc=N)` / `HUNG_AFTER_VERDICT` / `HUNG_NO_VERDICT` — the last two are what `ctest`
  cannot tell apart, and conflating them is what made #437 hard — and on a hang it captures `sample(1)`
  stacks plus any pending `SecurityAgent`. Use it after any change in this area:

  ```sh
  cmake -S src --preset dev -DCONTEXT_BUILD_GUI_CEF=ON
  cmake --build src/build/dev --target context_cef_boot_smoke context_gui_host \
      context_editor_shell_cef_smoke context_editor_shell_restore_smoke
  python3 tools/measure_cef_smoke_rate.py --build-dir src/build/dev -k 5
  ```

## Open, for the owner — the SHIPPED editor is not covered

`context_editor` keeps `use_mock_keychain = false` deliberately, and this task did not change that.
Two exposures remain, both product decisions rather than test hygiene:

1. **All CEF apps share one keychain item.** The service name comes from Chromium's branding, so every
   CEF-based app on a user's Mac contends for `"Chromium Safe Storage"`. The second one to run prompts.
   Chrome and Electron avoid this by setting their own product name; CEF exposes no such knob.
2. **An unsigned or ad-hoc-signed build prompts on every rebuild**, and if the user does not answer,
   the editor hangs on quit rather than exiting. A properly Developer-ID-signed app has a stable
   designated requirement, so it is added to the ACL once and stays — which makes this largely an e15
   signing matter, but only for signed builds.

A first-run keychain prompt is normal for a browser-based app (Chrome does it). A **hang** when the
prompt is ignored is not, and it is worth deciding whether the shipped editor should depend on the OS
key store at all: the Shell's default profile is an ephemeral per-PID cache dir, which has nothing
worth encrypting with a machine-persistent key.
