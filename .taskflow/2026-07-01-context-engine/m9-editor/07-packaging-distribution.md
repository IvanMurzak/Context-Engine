# 07 — Packaging & distribution

## 1. The application artifact

- **Name**: "Context Editor" (O2 default). One app per OS, engine release train (D14): editor
  version ≡ engine version ≡ `project(ContextEngine VERSION …)`; stamped into the About surface
  and `context.build.json`-style metadata.
- **Contents**: `context_editor` shell + CEF payload (minimal dist, prune unused locales ~50 MB)
  + editor-core static assets + tokens/themes + `context` CLI + EditorKernel daemon binary (the
  app SPAWNS the daemon — one install serves GUI/CLI/agents; D18).
- **Layout**: honors the R-VER-004 side-by-side `versions/<semver>/` convention so the v2
  launcher lands on an existing contract.

## 2. CEF sandbox — bootstrap launch model (closing the deferred item)

`USE_SANDBOX` is OFF today with bootstrap explicitly deferred (`ContextCef.cmake:77-78`). M9
turns the sandbox ON for the shipped app:

- **Windows**: CEF 149 (M138+) requires the `bootstrap.exe` launch model. CEF ships the
  bootstrap **unsigned by design** (cef#3935): we rename it (`context-editor.exe`) and sign
  the bootstrap + the client DLL **with the same certificate in one Artifact-Signing batch**
  (same-primary-cert rule; the DLL is named after the exe or passed via `--module`), and
  bootstrap↔libcef stay version-locked to the same CEF build (B-F4). Subprocesses spawn per
  CEF's model. CI smoke keeps a no-sandbox windowless mode (the existing `editor-cef-smoke`),
  but the PACKAGED app runs sandboxed and T2 boots the packaged shape; sandbox ON +
  `shared_texture_enabled` + windowless is explicitly asserted in ~~s2~~/T2, with a per-CEF-bump
  tripwire and automatic software-OSR degrade (B-F5, cef#4057).
  ⚠ **Amended 2026-07-19 — `s2` is SUPERSEDED** (owner rejected the wgpu-native fork; 03 §3).
  The assertion does **not** die with it: it re-homes wholly to **T2** (e15 packaging / e16 smoke
  job), which is where the packaged sandbox-ON shape exists anyway. Per-OS scope after the
  ruling: the **sandbox ON + windowless** half applies on every OS and is unchanged; the
  **`shared_texture_enabled`** half gates only where accelerated OSR is actually live — i.e.
  **macOS** — since Windows now ships the software-OSR / CPU-upload path. The per-CEF-bump
  tripwire and the automatic software degrade remain required on all OSes.
- **macOS**: standard `.app` + Helper bundles (already modeled in `host/CMakeLists.txt:61-113`)
  with the hardened runtime + the Chromium/V8 entitlement set (`allow-jit`,
  `allow-unsigned-executable-memory`, `disable-library-validation`) notarization requires;
  M138+ additionally ships the dynamic `libcef_sandbox.dylib` — helpers call
  `cef_sandbox_initialize()` before loading the framework (B-F12).
- **Linux**: CEF sandbox per upstream defaults for the packaged app.
- GPU-process and renderer flags: the current forced `disable-gpu` set (`editor_host.cpp:127-134`)
  is smoke-only; the app enables GPU compositing per the L-41 capability gate.

## 3. Installers (net-new; signing infra reused)

| OS | Artifact | Signing |
|---|---|---|
| Windows | MSI (WiX **v5** — the last fee-free line; v6/v7 require the Open Source Maintenance Fee → owner money-gate if chosen — B-F8) + portable ZIP | Azure **Artifact Signing** (renamed from "Trusted Signing", GA 2026-01 — B-F7) job (`release-sign.yml:119-202`) signs bootstrap + DLLs + MSI in one batch |
| macOS | `.dmg` (app bundle) | codesign (Developer-ID, hardened runtime) + notarytool `status==Accepted` + **staple the DMG** (finally possible — container exists; `docs/export-adapters.md:197-203`) |
| Linux | **`.deb` (the sandboxed channel)** + AppImage (convenience artifact; ⚠ cannot guarantee the Chromium sandbox on Ubuntu 23.10+ — the unprivileged-userns AppArmor restriction needs an installed profile an AppImage can't provide (B-F3); caveat stated on the download surface) + tar.gz | Ed25519 detached signature under the pinned trust root (`tools/trust-root/allowed_signers`), verified by `context doctor` |

- Installer build = a new packaging stage beside the export adapters (`src/editor/build/adapter.*`
  pattern); installer tools (WiX etc.) enter via the SHA-pinned `tools/*-prebuilt.json` +
  `fetch_*.py` convention (each new pinned tool re-enters the 08 §3 standing consent gate).
- `release-sign.yml` gets its artifact `paths` wired to the real installer outputs (today
  sample-only — `:20-21`); custody model B / environment-protected release stays.
- Ed25519 artifact signatures ship for ALL OS artifacts (belt over the OS-native braces),
  verify-before-use fail-closed (R-SEC-009).
- M9 exit requires signed installers EXISTING and boot-smoked; publishing them is the owner's
  separate release call (D16).

## 4. First-run & daemon lifecycle

- **Launch with project** (file association on the project marker / `context edit .` / "Open
  with"): shell resolves the project → reads `.editor/instance.json` → live daemon? attach :
  spawn daemon child (token via stdio) → window 0 restores layout.
- **Launch bare** (double-click, D13): **mini-welcome screen** in window 0 — recent projects
  (user config), "Open project…" (native folder picker), "New from template" (thin wrapper over
  `context new`, the R-QA-006 runnable templates). No engine-version management UI (v2,
  R-HUX-003).
- Daemon exit policy: daemon is a child of the FIRST editor process for the project but survives
  editor exit only if other clients hold attachments; otherwise clean shutdown (`shutdown` verb).
  A pre-existing external daemon is attached to, never owned.
- **Second project (D15)**: opening a different project from a running editor (recent item,
  file association) spawns a **new process** for that project; per-project single-instance is
  arbitrated via the project's `.editor/` state — an editor presence marker in
  `.editor/editor-state.json` lets the opener focus the existing process instead of spawning a
  duplicate (C-F23).
- **Auto-update (O3 default)**: notify-only banner comparing the running version against the
  latest published release (checked over HTTPS, no telemetry payload — a version string GET);
  click-through to the download page. Full in-app updater is post-M9.

## 5. Dev loop

- `cmake --preset dev` + `-DCONTEXT_BUILD_EDITOR=ON` builds shell+assets; editor-core supports a
  dev mode (esbuild watch + CEF reload command) for UI iteration without native rebuilds —
  matching the L-43 philosophy (native = rebuild-and-restart; web/TS = hot).
- Windows dev caveat inherited: MSVC-ABI prebuilts (CEF) — the GCC reference box cannot link the
  editor target; documented, CI covers it (01 §5).

## 6. Rollback / recovery

- Side-by-side versions (R-VER-004): installing N+1 never removes N; a bad editor build is
  exited by launching N (project files untouched — file-authoritative core means the editor
  carries no project-state risk).
- `.editor/session.json` (daemon-owned) and `.editor/editor-state.json` (editor-owned — 03 §1)
  are disposable by contract (session state): corrupt → renamed aside + defaults, loudly.
  Never blocks boot.
