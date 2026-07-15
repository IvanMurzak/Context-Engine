// The engine's embedded default fonts (M7 a7; issue #237). Compiled-in font bytes (no runtime file IO,
// no path resolution — the R-SEC-006 trust boundary: embedded trusted fonts only). The bytes are the
// EXACT vendored upstream TTFs (third_party/fonts/, generated into arrays by tools/embed_bin.py).
//
// LICENSE: both fonts are SIL Open Font License 1.1 (OFL-1.1), NOT the Context Engine EULA. Full
// provenance (version, source repo, SHA-256, no-Reserved-Font-Name verification) + the RUNTIME-RASTERIZE
// ONLY rule (never redistribute a pre-baked atlas) live in this package's README and
// tools/license-allowlist.json.

#pragma once

#include <cstddef>
#include <span>

namespace context::packages::ui::text
{

// Noto Sans Regular (Latin / Greek / Cyrillic), notofonts/latin-greek-cyrillic v2.015 — the default UI
// workhorse face.
[[nodiscard]] std::span<const std::byte> noto_sans_regular() noexcept;

// Noto Sans Arabic Regular, notofonts/arabic v2.013 — the complex-script face a8's RTL/ligature shaping
// DoD needs (a7 rasterizes its glyphs; shaping is a8).
[[nodiscard]] std::span<const std::byte> noto_sans_arabic_regular() noexcept;

} // namespace context::packages::ui::text
