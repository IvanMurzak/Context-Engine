# 08 — Security

## 1. Trust zones & inventory

| Zone | Holds | Never holds |
|---|---|---|
| Shell (native) | daemon socket, attach token, OS windows, GPU | — |
| editor-core (first-party web, per window) | scoped IPC bridge to Shell, command registry, layout | socket, token, Node, ambient FS |
| Built-in panel content | hydration DOM + kit | direct daemon access (goes through editor-core bridge) |
| Third-party panel (iframe) | sandboxed origin `context-ext://<pkg>`, postMessage bridge with GRANTED scopes only | socket, token, Node, cross-origin DOM, unscoped verbs |
| Daemon | project files, write queue, scope enforcement | UI trust decisions |

Credential inventory: attach token (`.editor/instance.json`, 0600/DACL; passed daemon→shell via
stdio on spawn); no other secrets exist in the editor. Signing keys live in CI environment
secrets / operator store (07) — never in the app.

## 2. Threat table

| Threat | Control |
|---|---|
| Malicious third-party panel exfiltrates project data | Sandboxed iframe, isolated renderer/origin, strict CSP (no external hosts), bridge scope enforcement IN THE DAEMON DISPATCHER (`dispatcher.cpp:203` model — adapters are bypassable, the dispatcher is not); default scope = read_query; file-write/build require install-time consent (L-49 surface); scheme registration pinned (`STANDARD\|SECURE\|CORS_ENABLED`), per-extension process isolation **verified in s1/T2** (Chromium `IsolateSandboxedIframes` is a feature default, not a CEF contract), bridge authenticated via MessageChannel ports (opaque-origin frames report origin `"null"` — origin strings are not authentication) (B-F6) |
| Hostile PROJECT content (authored strings: entity names, `notes`) smuggles markup/script into the trusted DOM via hydration (XSS) | `render_html` mandatory escaping contract on every text interpolation, asserted in T1 with adversarial project strings; strict no-inline-script CSP as backstop; iframe panels are origin-isolated regardless (C-F6) |
| Sandboxed panel observes editor telemetry / builds a cross-package channel via `editor.ui` | `editor.ui` read is a grantable manifest capability (`ui_events`), listed at install consent; custom topics are package-namespaced and manifest-declared; the bus is never forwarded to the daemon (D7) (C-F18) |
| Panel escalates via the bridge (calls `package add`, `build`) | Method→scope table (`scope.h:62-76`); `build_install` never granted to panels by default; scope-denied is a tested T1/T2 assertion (the existing editor_host smoke already asserts this — kept) |
| Rogue local process attaches to the daemon | D20: token verified at attach + pipe owner-SID DACL (Win) / 0600 socket (POSIX); wrong token → `attach.denied`; `clients` topic makes attachments observable |
| Token leakage via web layer | Token is Shell-only (04 §1); editor-core/panels use the brokered IPC; CSP forbids external network so a stolen bridge still can't phone home |
| Compromised CEF renderer process | CEF sandbox ON in the packaged app (07 §2); renderer holds no token/socket; brokered IPC is scope-checked server-side; crash recovery is a designed drill (03 §7) |
| Malicious theme file | Themes are schema-validated JSON DATA (D11) — no CSS/JS execution vector; unknown keys rejected; package themes ride package trust (L-49) |
| Malicious keymap file | Schema-validated; bindings can only reference registered command ids; commands are scope-checked downstream |
| Supply chain: Dockview + editor-core npm deps | Deny-by-default license allowlist (existing CI gate) + lockfile-pinned, vendored-at-build deps; bundling via the SHA-pinned esbuild; NEW: npm deps for editor-core enter the same review gate as vcpkg deps (new-dep = consent-gated change class) |
| Supply chain: CEF/wgpu prebuilts | Existing SHA-256 pin + fetch-verify fail-closed (`tools/cef-prebuilt.json`, `fetch_cef.py`) — unchanged |
| Tampered installer | OS-native signatures (Authenticode / notarized DMG) + Ed25519 detached signatures under the pinned trust root; verify-before-use fail-closed (R-SEC-009); T2 boots the SIGNED artifact |
| Drag/drop or file-open of hostile paths | All file access via daemon verbs under project scoping; the welcome screen's recent list stores paths only |
| Update-check privacy | Notify-only version GET, no identifiers (O3); no telemetry anywhere (L-57 contractual-only enforcement posture extends to the editor) |

## 3. Security gates in the rollout

- New npm dependency set (Dockview + minimal deps) → **human approval gate** at s1 ratification
  (supply-chain review: maintenance, transitive tree, license).
  ✅ **CLEARED 2026-07-19 — owner approved `dockview-core@7.0.2`** (MIT, **0 runtime deps**,
  framework-agnostic core; published 2026-06-22), off s1's supply-chain review. Correction to
  the design's original assumption: the editor needs **exactly ONE package**, not the
  core + ~~`dockview-modules`~~ set 02/04 assumed. ⚠ The approval is **VERSION-PINNED**: a bump
  past `7.0.2`, **or** pulling in any additional `dockview-*` package, re-triggers the standing
  consent gate below. This is not blanket Dockview approval.
- **Standing change-class gate**: every LATER new pinned dep/prebuilt (WiX, Node/bundler pins,
  a Dockview-fallback swap, ~~the patched wgpu-native fork~~) re-enters the same consent gate —
  not a one-time s1 event (C-F17).
  ⛔ **RULED 2026-07-19 — the patched `wgpu-native` fork was REJECTED by the owner**, on the
  grounds that carrying a fork is an unbounded long-term maintenance cost (every upstream
  rebase, forever). It is therefore struck from this list not because it stopped being
  consent-gated, but because it is **decided and closed**: **no forked or patched wgpu-native
  prebuilt may enter the tree.** Any future attempt to reintroduce one is a fresh owner
  decision, not a routine consent-gate item. The stock, SHA-pinned wgpu-native supply chain
  (§2, "Supply chain: CEF/wgpu prebuilts") stands unchanged. Design impact: 03 §3;
  revisit trigger (upstream C-API import) tracked in the ROADMAP Backlog.
- Sandbox-ON packaging (07 §2) is a REQUIRED M9 exit criterion **per OS channel**: MSI/ZIP,
  `.dmg`, and `.deb` ship sandboxed; the Linux AppImage is a stated-caveat convenience
  artifact (Ubuntu 23.10+ userns/AppArmor reality — B-F3) and never the default download.
- Scope-denial tests (panel cannot write/build without grant) are blocking T1+T2 gates.
- The hostile-extension red-team remains v2 (R-EDIT-001 v1 hardening scope) — recorded as an
  accepted, named residual; mechanical clamps (sandbox, CSP, scopes, origin isolation) all ship
  and are enforced in M9.
