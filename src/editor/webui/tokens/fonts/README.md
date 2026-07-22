# Vendored fonts — Geist + Geist Mono (M9 e06a, design 06 § 2)

The two typefaces the `typography` token group names: **Geist** (UI) and **Geist Mono** (data / ids /
badges). Vendored as **variable** woff2 (one file per family covers the whole weight axis, so the token
weight scale — regular 430 / medium 520 / semibold 600 — renders exactly, with no per-weight faces).

| File | Family | sha256 |
|---|---|---|
| `Geist-Variable.woff2` | Geist (UI), variable `wght` 100–900 | `28258d0621216948416a859d32487ab6ad1c9effa0d08795698e70be3c917630` |
| `GeistMono-Variable.woff2` | Geist Mono (mono), variable `wght` 100–900 | `5bc6413e82be410dc057feccee55160495b999d0fe212b7b6c6499b29b8b1e4a` |

## Provenance & license

- **Version**: Geist `1.3.1` (the `geist` npm distribution, `dist/fonts/geist-sans` + `geist-mono`).
- **Upstream**: <https://github.com/vercel/geist-font> — Copyright (c) 2023 Vercel, in collaboration
  with basement.studio.
- **License**: **SIL Open Font License 1.1 (OFL-1.1)**, vendored verbatim beside the fonts as
  [`OFL.txt`](OFL.txt). No Reserved Font Name, so redistribution + subsetting are permitted. The fonts
  are **DATA under OFL-1.1**, NOT the Context Engine EULA.
- These are the FULL unmodified upstream variable faces (not a subset), so they are **not** a Modified
  Version. This mirrors the vendored-font provenance discipline already recorded for the FreeType text
  stack's Noto faces (`tools/license-allowlist.json` `$comment`). The deny-by-default code-license gate
  (`tools/check_licenses.py`) scans declared `package.json` / `vcpkg.json` dependencies only, so it does
  not see these data files, and OFL-1.1 is deliberately **not** on `allowed_licenses` (which governs
  code linked into the engine core).

## Not yet wired

`@font-face` declarations, the served-asset staging of these woff2, and pointing the `typography`
tokens' font stacks at the local faces are the theme ENGINE's job (**e06b**). This task only vendors the
files as the DATA the typography tokens reference.
