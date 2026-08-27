---
id: e15-packaging-installers
title: Packaging — CEF sandbox ON (bootstrap model), signed installers per OS, release-sign wiring, Ed25519 belt-and-braces
group: E
sequence: 1
repo: "."
base_branch: "main"
depends_on: [e04-window-shell-windows, e13-package-panels]
importance: 9
complexity: 7
security_critical: true   # signing, sandbox, supply-chain-pinned installer tools
production_touching: true
model_hint: top           # mid by complexity, bumped for security
taskflow_refs: [07, 08, 01]
---

## Goal

Turn the editor into a shippable, signed, sandboxed desktop application: CEF sandbox ON via
the per-OS launch models, MSI/dmg/deb(+AppImage) installers built by a new packaging stage,
`release-sign.yml` wired to real artifacts, and Ed25519 signatures under the pinned trust
root on everything (R-SEC-009).

## Scope & seams

- **Sandbox ON** (today `USE_SANDBOX OFF` — `ContextCef.cmake:91`, bootstrap deferred
  `:75-78`):
  - **Windows** (B-F4): CEF 149 bootstrap launch model — rename bootstrap →
    `context-editor.exe`; sign bootstrap + client DLL **with the same certificate in ONE
    Artifact-Signing batch** (same-primary-cert rule; DLL named after exe or `--module`);
    bootstrap↔libcef version-locked to the same CEF build.
  - **macOS** (B-F12): `.app` + Helper bundles (existing model `host/CMakeLists.txt:61-113`)
    + hardened runtime + Chromium/V8 entitlements (`allow-jit`,
    `allow-unsigned-executable-memory`, `disable-library-validation`); M138+ dynamic
    `libcef_sandbox.dylib` — helpers call `cef_sandbox_initialize()` before framework load.
  - **Linux**: upstream sandbox defaults; **`.deb` = the sandboxed channel**; AppImage =
    stated-caveat convenience artifact (Ubuntu 23.10+ userns/AppArmor — B-F3), NEVER the
    default download; + tar.gz.
  - Packaged app enables GPU per the L-41 gate (the smoke-only `disable-gpu` set
    `editor_host.cpp:127-134` stays smoke-only).
- **App contents** (07 §1): shell + CEF payload (prune locales ~50 MB) + editor-core assets
  + tokens/themes + `context` CLI + daemon binary (one install serves GUI/CLI/agents — D18);
  R-VER-004 `versions/<semver>/` side-by-side layout; version ≡ engine version (D14),
  stamped in About + build metadata; file-association registration (e14 handler).
- **Installer stage**: beside the export adapters (`src/editor/build/adapter.*` pattern);
  **WiX v5** (last fee-free line; v6/v7 = owner money-gate — B-F8); installer tools enter
  via SHA-pinned `tools/*-prebuilt.json` + `fetch_*.py` — **each new pinned tool re-enters
  the 08 §3 standing consent gate**.
- **Signing** (`release-sign.yml` — artifact `paths` today sample-only `:18-21`): Azure
  **Artifact Signing** (renamed from "Trusted Signing" — B-F7) Windows batch; codesign +
  notarytool `status==Accepted` + **staple the DMG** (container finally exists —
  `docs/export-adapters.md:197-203`); custody model B / environment-protected stays.
- **Ed25519 detached signatures** for ALL OS artifacts under the pinned trust root
  (`tools/trust-root/allowed_signers`), verify-before-use fail-closed; `context doctor`
  verifies.

## Definition of Done

- [ ] Owner consent obtained for each new pinned tool (WiX etc.) BEFORE it lands (08 §3)
- [ ] Signed MSI + portable ZIP / stapled DMG / `.deb` + AppImage + tar.gz produced by CI
      (release-sign dry-run; real signing on the environment-protected path)
- [ ] Clean-host boot smoke per artifact (M9 exit clause 2 shape); sandbox ON verified in
      the packaged shape on all three OSes
- [ ] Ed25519 signatures verify fail-closed; `context doctor` reports them
- [ ] AppImage caveat stated on the artifact surface; `.deb` is the default Linux download
- [ ] Version/About stamped ≡ engine version; R-VER-004 layout honored; 3-OS CI green
