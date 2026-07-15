// Accessors for the embedded default fonts. The byte arrays are generated from the vendored TTFs by
// tools/embed_bin.py (see this package's CMakeLists add_custom_command); this file only wraps them as
// std::span. See embedded_fonts.h for provenance/licensing.

#include "context/packages/ui/text/embedded_fonts.h"

namespace context::packages::ui::text::detail
{
// Defined by the generated TUs (tools/embed_bin.py). `extern` + the generated initializer give external
// linkage matching these declarations.
extern const unsigned char kNotoSansRegularTtf[];
extern const unsigned long kNotoSansRegularTtf_len;
extern const unsigned char kNotoSansArabicRegularTtf[];
extern const unsigned long kNotoSansArabicRegularTtf_len;
} // namespace context::packages::ui::text::detail

namespace context::packages::ui::text
{

std::span<const std::byte> noto_sans_regular() noexcept
{
    return {reinterpret_cast<const std::byte*>(detail::kNotoSansRegularTtf),
            static_cast<std::size_t>(detail::kNotoSansRegularTtf_len)};
}

std::span<const std::byte> noto_sans_arabic_regular() noexcept
{
    return {reinterpret_cast<const std::byte*>(detail::kNotoSansArabicRegularTtf),
            static_cast<std::size_t>(detail::kNotoSansArabicRegularTtf_len)};
}

} // namespace context::packages::ui::text
