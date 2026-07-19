# npm supply-chain review — Dockview v7 (owner allowlist gate, M9 s1)

Filed for the **human approval gate at s1 ratification** (`08-security.md` §3: "New npm dependency
set (Dockview + minimal deps) -> human approval gate at s1 ratification (supply-chain review:
maintenance, transitive tree, license)"). Covers the exact package the ratification recommends
pinning. **Recommendation: APPROVE `dockview-core@7.0.2` for the editor-core npm allowlist.**

## 1. Dependency set under review

| | |
|---|---|
| Package | **`dockview-core`** |
| Version | **7.0.2** (published 2026-06-22; latest, and the only 7.x on npm) |
| Purpose | The M9 editor docking engine (design D2) — framework-agnostic core, vanilla `createDockview()` |
| npm integrity | `sha512-i666ndkUdT4U+Bo2Yu3d6KwCJNqe21VJ0oschdgcXucZ5TquzBFX94zcUBEfg4IewTv2BuiPJ7r/2JTGLLv6ig==` |
| tarball sha256 | `e75f201ed9a59346274a333cfcebc56cccf5b11a576b008910cb063ed0ecf5eb` |
| Repository | https://github.com/mathuo/dockview |

**This is the whole set.** The spike ratification (FINDINGS.md § Package set) corrected the design's
assumption of a "`dockview` core + `dockview-modules` (incl. a11y module)" split: no such separate
packages exist. v7 ships every feature module (accessibility, floating-group, popout,
keyboard-docking, live-region, …) **inside `dockview-core`**, auto-registered. So the editor-core
depends on **one** package, not a set. The framework bindings (`dockview`, `dockview-react`,
`dockview-vue`, `dockview-angular`) are NOT used.

## 2. Transitive dependency tree

**Empty.** `dockview-core@7.0.2` declares:

- `dependencies`: **none** (0)
- `peerDependencies`: **none** (0)
- `optionalDependencies`: **none** (0)

npm's own package description is literally "Zero dependency layout manager." The transitive attack
surface added to the editor-core supply chain is therefore **exactly the one reviewed package** — no
hidden install-script deps, no transitive CVE inheritance, nothing else to vet. This is the
best-possible outcome for a deny-by-default posture and materially de-risks the standing change-class
gate (`08` §3 C-F17): future Dockview upgrades cannot silently pull a new transitive dependency
without a visible `dependencies` change in a new version.

## 3. License

| | |
|---|---|
| Declared license | **MIT** (`package.json` `"license": "MIT"`; UMD banner `@license MIT`) |
| On `allowed_licenses`? | **Yes** — `MIT` is already in `tools/license-allowlist.json` `allowed_licenses` |
| Notice completeness | The npm tarball ships `README.md` but **no `LICENSE` file**; the MIT grant is declared in `package.json` + the minified banner. A shipped-editor third-party-notice pass (an F0b/packaging concern, mirroring the CEF notice handling in `tools/cef-prebuilt.json`) should capture the MIT text from the GitHub repo's `LICENSE`. |

No copyleft, no source-available/commercial clause, no CLA requirement for *use*. MIT is compatible
with the Context Engine EULA distribution.

## 4. Maintenance health

| Signal | Finding |
|---|---|
| Age / maturity | Created 2023-03-25 — ~3+ years, well past the "new package" risk window |
| Release cadence | Very active: 6.4.0 -> 6.5.0 -> 6.6.0 -> 6.6.1 (May 2026), 7.0.2 (2026-06-22), an `experimental` dist-tag rolling weekly |
| Ecosystem footprint | The de-facto TS docking library (framework bindings for React/Vue/Angular; third-party Solid/Svelte ports exist), used widely for IDE-like web UIs |
| **Bus factor** | **Single maintainer** (`mathuo`, the author). This is the primary residual risk. |
| Build integrity | The vendored UMD bundle contains **no `eval(` / `new Function(`** — it runs under a strict `script-src 'self'` CSP with no `unsafe-eval` (FINDINGS probe 1), so a compromised build could not smuggle string-eval'd code past the editor-core CSP |

## 5. Risk assessment + mitigations

- **Single-maintainer bus factor (primary flag).** Mitigations already in the design: (a) the
  dependency is **vendored + SHA-pinned** and **bundled at build via the SHA-pinned esbuild** (`04`
  §1) — no live npm fetch at runtime or in CI, so an upstream account compromise cannot reach a
  shipped build without a reviewed version bump passing this same gate again; (b) a **documented
  fallback** exists and was kept ready (Golden Layout -> Lumino, `04` §2 D2 order) should the project
  stall; (c) the zero-dependency tree means a maintainer lapse never cascades through transitives.
- **Version-pin discipline.** Pin the exact `7.0.2` (not `^7`), verify-before-use against the hashes
  in §1, and route every future bump through the standing change-class gate (`08` §3 C-F17).
- **Notice completeness.** Fold the MIT `LICENSE` text into the editor-core third-party notices at
  packaging (F0b), alongside the CEF/Chromium notices.

## 6. Recommendation to the owner

**APPROVE** `dockview-core@7.0.2` (MIT, zero-dependency) as the sole Dockview npm dependency for
editor-core. Residual risk is the single-maintainer bus factor, mitigated by vendoring + SHA-pin +
build-time bundling + a ready fallback.

**Allowlist action (defer to the editor-core landing, not this spike).** When the M9 editor-core npm
workspace (`src/editor/webui/`, `04` §1) is created and declares `dockview-core` in its `package.json`,
add to `tools/license-allowlist.json`:

```jsonc
"dependency_licenses": { …, "dockview-core": "MIT" }   // MIT already in allowed_licenses
```

so the deny-by-default `license-gate` (which scans every `package.json` in the repo) passes. **This
spike deliberately does NOT make that edit**: it vendors the assets with no `package.json`, so the
`license-gate` is untouched and this owner ratification is not pre-empted by an automated commit.
Adding the allowlist entry now — before editor-core exists — would allowlist a dependency the repo
does not yet declare.
