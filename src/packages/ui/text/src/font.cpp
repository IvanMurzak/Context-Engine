// FreeType-backed FontFace implementation (the ONLY TU that includes FreeType — the header is
// FreeType-free via PIMPL). Elects the FTL license limb; the build disables every optional FreeType
// subsystem (see cmake/ContextFreetype.cmake), so this rasterizes embedded outline fonts and nothing
// else. R-SEC-006: faces come only from embedded trusted bytes.

#include "context/packages/ui/text/font.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>

namespace context::packages::ui::text
{
namespace
{
// Round a float pixel size to the integer FreeType pixel size (min 1).
FT_UInt to_pixel_size(float pixel_size) noexcept
{
    long r = std::lround(pixel_size);
    if (r < 1)
        r = 1;
    return static_cast<FT_UInt>(r);
}
} // namespace

struct FontFace::Impl
{
    FT_Library lib = nullptr;
    FT_Face face = nullptr;

    ~Impl()
    {
        if (face != nullptr)
            FT_Done_Face(face);
        if (lib != nullptr)
            FT_Done_FreeType(lib);
    }
};

FontFace::FontFace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FontFace::FontFace(FontFace&&) noexcept = default;
FontFace& FontFace::operator=(FontFace&&) noexcept = default;
FontFace::~FontFace() = default;

std::optional<FontFace> FontFace::from_memory(std::span<const std::byte> ttf)
{
    if (ttf.empty())
        return std::nullopt;

    auto impl = std::make_unique<Impl>();
    if (FT_Init_FreeType(&impl->lib) != 0)
        return std::nullopt;

    const FT_Error err = FT_New_Memory_Face(impl->lib,
                                            reinterpret_cast<const FT_Byte*>(ttf.data()),
                                            static_cast<FT_Long>(ttf.size()), 0, &impl->face);
    if (err != 0 || impl->face == nullptr)
        return std::nullopt;

    // Prefer a Unicode charmap; FT_New_Memory_Face usually selects one already, so ignore the result.
    (void)FT_Select_Charmap(impl->face, FT_ENCODING_UNICODE);
    return FontFace(std::move(impl));
}

GlyphId FontFace::glyph_index(char32_t codepoint) const
{
    return static_cast<GlyphId>(FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(codepoint)));
}

FontMetrics FontFace::metrics(float pixel_size) const
{
    FontMetrics m;
    if (FT_Set_Pixel_Sizes(impl_->face, 0, to_pixel_size(pixel_size)) != 0)
        return m;
    const FT_Size_Metrics& sm = impl_->face->size->metrics;
    m.ascent = static_cast<float>(sm.ascender) / 64.0f;
    m.descent = static_cast<float>(sm.descender) / 64.0f; // negative (below baseline)
    m.line_height = static_cast<float>(sm.height) / 64.0f;
    m.line_gap = m.line_height - (m.ascent - m.descent);
    return m;
}

float FontFace::advance(GlyphId glyph, float pixel_size) const
{
    if (FT_Set_Pixel_Sizes(impl_->face, 0, to_pixel_size(pixel_size)) != 0)
        return 0.0f;
    if (FT_Load_Glyph(impl_->face, static_cast<FT_UInt>(glyph), FT_LOAD_DEFAULT) != 0)
        return 0.0f;
    return static_cast<float>(impl_->face->glyph->advance.x) / 64.0f;
}

std::optional<GlyphBitmap> FontFace::rasterize(GlyphId glyph, float pixel_size) const
{
    if (FT_Set_Pixel_Sizes(impl_->face, 0, to_pixel_size(pixel_size)) != 0)
        return std::nullopt;
    if (FT_Load_Glyph(impl_->face, static_cast<FT_UInt>(glyph), FT_LOAD_RENDER) != 0)
        return std::nullopt;

    const FT_GlyphSlot slot = impl_->face->glyph;
    GlyphBitmap out;
    out.advance = static_cast<float>(slot->advance.x) / 64.0f;
    out.left = slot->bitmap_left;
    out.top = slot->bitmap_top;

    const FT_Bitmap& bm = slot->bitmap;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    if (out.width > 0 && out.height > 0 && bm.buffer != nullptr)
    {
        out.coverage.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
        const int pitch = bm.pitch; // >= 0 top-down; < 0 bottom-up
        for (int y = 0; y < out.height; ++y)
        {
            const int row = (pitch >= 0) ? y : (out.height - 1 - y);
            const unsigned char* src = bm.buffer + static_cast<std::ptrdiff_t>(row) * std::abs(pitch);
            std::uint8_t* dst = out.coverage.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width);
            std::copy(src, src + out.width, dst);
        }
    }
    return out;
}

const void* FontFace::identity() const noexcept
{
    return impl_->face;
}

std::string FontFace::family_name() const
{
    return (impl_->face->family_name != nullptr) ? std::string(impl_->face->family_name)
                                                 : std::string();
}

std::size_t FontFace::glyph_count() const noexcept
{
    return static_cast<std::size_t>(impl_->face->num_glyphs);
}

} // namespace context::packages::ui::text
