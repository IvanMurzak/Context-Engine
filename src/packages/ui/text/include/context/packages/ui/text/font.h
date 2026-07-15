// A loaded font face over FreeType (M7 a7, R-UI-005 / R-SEC-006; issue #237). Move-only RAII around a
// FreeType FT_Face via PIMPL, so FreeType NEVER leaks into this header — consumers (and the render/ui
// atlas + emitter) stay FreeType-free and this header compiles on every toolchain with no FT include.
//
// Trust boundary (R-SEC-006, v1): faces come ONLY from embedded trusted font bytes (embedded_fonts.h);
// there is NO user-suppliable font asset kind yet (the fuzz-hardening follow-up stays declared).

#pragma once

#include "context/packages/ui/text/glyph.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace context::packages::ui::text
{

// Vertical line metrics at a pixel size (pixels). `ascent` is the distance baseline->top (positive up);
// `descent` is baseline->bottom (negative, i.e. below the baseline); `line_height` is the recommended
// baseline-to-baseline advance (ascent - descent + line_gap).
struct FontMetrics
{
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_gap = 0.0f;
    float line_height = 0.0f;
};

class FontFace
{
public:
    // Load a face from in-memory font bytes. The bytes MUST outlive the face (FreeType reads them
    // lazily) — the embedded default fonts are static, so this always holds. Returns nullopt if
    // FreeType rejects the data (not a valid sfnt/TrueType) or the library fails to initialize.
    [[nodiscard]] static std::optional<FontFace> from_memory(std::span<const std::byte> ttf);

    FontFace(FontFace&&) noexcept;
    FontFace& operator=(FontFace&&) noexcept;
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;
    ~FontFace();

    // The glyph id for a Unicode codepoint, or kMissingGlyph (.notdef) if the face has no mapping.
    [[nodiscard]] GlyphId glyph_index(char32_t codepoint) const;

    // Vertical metrics at `pixel_size` (rounded to an integer pixel size, min 1).
    [[nodiscard]] FontMetrics metrics(float pixel_size) const;

    // Horizontal pen advance of a glyph at `pixel_size` (pixels). 0 for an invalid glyph.
    [[nodiscard]] float advance(GlyphId glyph, float pixel_size) const;

    // Rasterize a glyph to an 8-bit alpha coverage bitmap at `pixel_size`. A whitespace glyph yields an
    // empty bitmap (width == height == 0) with a valid advance. nullopt on a FreeType load/render error.
    [[nodiscard]] std::optional<GlyphBitmap> rasterize(GlyphId glyph, float pixel_size) const;

    // A stable opaque identity for atlas keying — distinct faces compare unequal, the same face is
    // stable across calls. (The underlying FT_Face pointer; never dereferenced by consumers.)
    [[nodiscard]] const void* identity() const noexcept;

    // The face's family name (from its `name` table), for diagnostics + tests. Empty if unavailable.
    [[nodiscard]] std::string family_name() const;

    // The number of glyphs the face contains.
    [[nodiscard]] std::size_t glyph_count() const noexcept;

private:
    struct Impl;
    explicit FontFace(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::packages::ui::text
