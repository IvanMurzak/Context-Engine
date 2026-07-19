// Vocabulary shared by the whole presentation layer (M9 e03) — the host platform, and the one rule
// deciding whether a strided CPU buffer really contains the rows it claims.
//
// It lives in its own header because BOTH halves of the layer need it and neither should have to
// include the other to get it: the CPU present fallback (present_blit.h) has nothing to do with OSR
// import, yet pulling PresentPlatform out of osr_import.h would drag OsrFrame, OsrImportOptions and
// OsrTextureImporter along with it.

#pragma once

#include "context/render/rhi.h"

#include <cstdint>

namespace context::render::present
{

// The host platform of the present path. A LOCAL enum on purpose: src/render/ must not depend on the
// editor tree, so this deliberately does not reuse editor::gui::compositor::HostPlatform. The two are
// kept in correspondence by the Shell, which owns both. `linux_` carries a trailing underscore
// because a GNU-dialect compiler predefines `linux` to 1 and would mangle the enumerator.
enum class PresentPlatform
{
    windows,
    macos,
    linux_,
};

// The compile-time host — the ONLY `#if` in this seam, and trivial. Every policy function takes the
// platform EXPLICITLY so all three branches run in the tests on every OS.
[[nodiscard]] PresentPlatform current_present_platform();

[[nodiscard]] const char* to_string(PresentPlatform platform);

// Does a buffer of `available` bytes hold `size.height` rows of `size.width` BGRA8 pixels at
// `bytes_per_row` stride? Only the last row's PIXELS need be present — trailing stride padding on it
// is optional, which is why this is not simply `height * bytes_per_row`.
//
// ONE definition, because both readers of a producer's buffer depend on it: the dirty-rect upload
// driver (per sub-rect, at an offset) and the present blitters (whole image). Two copies of this
// arithmetic is two chances for an out-of-bounds read to be introduced by drift.
[[nodiscard]] inline bool rows_fit(Extent2D size, std::uint32_t bytes_per_row, std::size_t available)
{
    if (is_empty(size) || bytes_per_row < size.width * 4u)
    {
        return false;
    }
    const std::size_t needed = static_cast<std::size_t>(size.height - 1u) * bytes_per_row +
                               static_cast<std::size_t>(size.width) * 4u;
    return available >= needed;
}

} // namespace context::render::present
