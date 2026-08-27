# a7/a8 supply-chain + font gate — DECISION RECORD

> **VERDICT: GO** (approved as specified — no changes to the stack). Rendered 2026-07-15 by an
> autonomous **Fable decision agent** with full access, to which the owner explicitly delegated
> this gate ("Создай под-агента с моделью Fable … пусть он решит"). Ground truth verified against
> upstream LICENSE texts, vcpkg SPDX records, the actual OFL files, the Context-Engine EULA
> (`LICENSE.md` §2(3)/§3(4)), and the deny-by-default gate (`tools/check_licenses.py`).
> **These conditions are BINDING and MUST be folded into the a7 and a8 dispatch briefs.**

## Dependencies — all approved

| Dep | Verified SPDX | Allowlist action |
|---|---|---|
| **FreeType** | Dual `FTL OR GPL-2.0-or-later` → **elect `FTL`** (never the GPL limb) | ADD `"FTL"` to `allowed_licenses`; row `"freetype":"FTL"`; record FTL-over-GPL election in `$comment` |
| **HarfBuzz** | `MIT-Modern-Variant` ("Old MIT" — NOT plain MIT) | ADD `"MIT-Modern-Variant"` to `allowed_licenses`; row `"harfbuzz":"MIT-Modern-Variant"` |
| **SheenBidi** | `Apache-2.0` (already allowlisted) | row `"sheenbidi":"Apache-2.0"` only |
| **libunibreak** | `Zlib` (already allowlisted) | row `"libunibreak":"Zlib"` only |

→ Exactly **two** `allowed_licenses` adds: `FTL`, `MIT-Modern-Variant`. **Do NOT add `OFL-1.1` to
`allowed_licenses`** (font license, not a code license — provenance row only).

## Embedded fonts — blessed (notofonts project, OFL-1.1, verified NO Reserved Font Name)

- **Latin workhorse:** **Noto Sans Regular** (`notofonts/latin-greek-cyrillic`; covers Greek/Cyrillic too). Inter (OFL, no RFN) acceptable substitute.
- **Complex-script test font:** **Noto Sans Arabic Regular** (`notofonts/arabic`) — strongest single test for a8's joining/ligature/marks/RTL DoD. **Noto Sans Hebrew** optional 2nd RTL.
- No RFN on these → **subsetting is permitted** (still a "Modified Version": keep copyright+OFL, document the subset tool/options in the provenance row).

## Binding conditions for the implementer (fold into a7 + a8 briefs)

1. **Allowlist/SBOM:** the two `allowed_licenses` adds + four `dependency_licenses` rows exactly as tabled; `$comment` gets the FTL-election narrative + miniaudio-style font provenance rows (name, version, source repo, OFL-1.1, no-RFN note, no-prebaked-atlas rule). License gate + SBOM green with new entries (both DoDs).
2. **Delivery = vendored source OR SHA-pinned fetch ONLY** (`cmake/ContextDownload.cmake` / `tools/fetch_*.py` + pin manifest, verify-before-use, fail closed). **NOT `src/vcpkg.json`** (inert on default preset + all 3 CI legs). HarfBuzz: use the amalgamated single-TU `src/harfbuzz.cc`; SheenBidi/libunibreak are small C libs (miniaudio precedent); FreeType via its first-class CMake.
3. **FreeType build config:** disable optional subsystems — `FT_DISABLE_ZLIB/BZIP2/PNG/HARFBUZZ/BROTLI` — to sever the FreeType→HarfBuzz circular option and minimize surface (embedded raw TTFs need none).
4. **Fonts:** runtime-rasterize ONLY — **never redistribute pre-baked atlases** (a7 rule stands). Vendor each TTF beside its upstream OFL.txt + copyright line, pinned to a named notofonts release (version + SHA-256). README states fonts remain OFL-1.1, not covered by the Engine EULA.
5. **Notices:** vendor each library's upstream LICENSE/COPYING **verbatim** in `third_party/<name>/`. ui-package README documents (a) the FTL product-documentation credit that flows down to shipped Products, (b) OFL font provenance + rules. Extend the existing aggregate THIRD-PARTY-NOTICES packaging task to cover the text stack — do NOT block a7/a8 on it.
6. **Trust boundary (v1):** embedded trusted fonts ONLY — no user-suppliable font asset kind (R-SEC-006 fuzz follow-up stays declared, per a7).

## Rationale / residual risk (accepted)
FreeType+HarfBuzz = the Chrome/Android/Linux text stack (oss-fuzz-hardened); SheenBidi actively maintained (Unicode 17.0). Both design substitutions affirmed: **SheenBidi-over-FriBidi** (avoids LGPL relink in static proprietary cores) and **FreeType-over-stb_truetype** (hardening + survives the user-font follow-up). EULA §2(3) permits Product redistribution of runtime components; §3(4) notice-retention matches FTL/Apache/MIT-Modern-Variant duties. Residual: HarfBuzz sub-dir COPYING files (mitigated by amalgamated build + check-on-vendor); EULA is counsel-unreviewed draft (standing item, not a blocker); FTL doc-credit is a downstream-licensee duty the engine documents but can't enforce (same as Unity/Unreal/Godot).
