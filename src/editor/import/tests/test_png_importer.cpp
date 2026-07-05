// PNG importer: structural parse (happy + edge) + CRC/failure paths + descriptor shape.

#include "context/editor/import/import_settings.h"
#include "context/editor/import/importers/png_importer.h"
#include "context/editor/import/platform_profile.h"

#include "descriptor_read.h"
#include "import_test.h"

#include <string>
#include <vector>

using namespace context::editor::import;
using context::editor::serializer::JsonValue;

namespace
{
bool has_code(const std::vector<ImportDiagnostic>& diags, const char* code)
{
    for (const ImportDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}
} // namespace

int main()
{
    const PngImporter importer;
    const PlatformProfile win = *find_platform_profile("windows");
    const ImportSettings settings = resolve_import_settings("", "windows");

    // Happy path: valid RGBA -> full descriptor.
    {
        const std::string png = importtest::make_png(4, 3, 6);
        PngInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_png(png, info, diags));
        CHECK(diags.empty());
        CHECK(info.width == 4);
        CHECK(info.height == 3);
        CHECK(info.color_type == 6);
        CHECK(info.channels == 4);
        CHECK(info.has_alpha);

        ImportInput in;
        in.source_path = "hero.png";
        in.source_bytes = png;
        in.settings = settings;
        in.platform = win;
        const ImportResult r = importer.import(in);
        CHECK(r.ok);
        CHECK(r.artifacts.size() == 1);
        CHECK(r.artifacts[0].kind == ArtifactKind::texture);
        CHECK(r.artifacts[0].name == "texture");
        CHECK(r.artifacts[0].derived_format_version == 1);

        const auto desc = importtest::parse_descriptor(r.artifacts[0].bytes);
        CHECK(importtest::is_object(desc));
        CHECK(importtest::sfield(desc, "kind") == "texture");
        CHECK(importtest::sfield(desc, "source") == "png");
        CHECK(importtest::ifield(desc, "width") == 4);
        CHECK(importtest::ifield(desc, "height") == 3);
        CHECK(importtest::ifield(desc, "channels") == 4);
        CHECK(importtest::bfield(desc, "hasAlpha"));
        const JsonValue* transcode = importtest::ofield(desc, "transcode");
        CHECK(transcode != nullptr);
        CHECK(importtest::sfield(*transcode, "platform") == "windows");
        CHECK(importtest::sfield(*transcode, "format") == "bc7"); // desktop block-compressed target
    }

    // Edge: grayscale (color type 0) -> 1 channel, no alpha; palette (3) -> 1 index channel.
    {
        PngInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_png(importtest::make_png(2, 2, 0), info, diags));
        CHECK(info.channels == 1);
        CHECK(!info.has_alpha);
        PngInfo pal;
        diags.clear();
        CHECK(parse_png(importtest::make_png(2, 2, 3), pal, diags));
        CHECK(pal.channels == 1);
    }

    // Edge: an sRGB chunk is recorded as colorspace provenance.
    {
        PngInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_png(importtest::make_png(2, 2, 6, 8, 0, /*with_srgb=*/true), info, diags));
        CHECK(info.srgb);
    }

    // Failure paths (each is refused with a specific catalog code, never a throw).
    {
        PngInfo info;
        std::vector<ImportDiagnostic> diags;

        CHECK(!parse_png("not a png at all!!", info, diags)); // bad signature
        CHECK(has_code(diags, "import.source_malformed"));

        diags.clear();
        const std::string good = importtest::make_png(4, 4, 6);
        CHECK(!parse_png(good.substr(0, 20), info, diags)); // truncated mid-IHDR
        CHECK(has_code(diags, "import.source_malformed"));

        diags.clear();
        std::string corrupt = importtest::make_png(4, 4, 6);
        corrupt[20] = static_cast<char>(corrupt[20] ^ 0xFF); // flip an IHDR-data byte -> CRC fails
        CHECK(!parse_png(corrupt, info, diags));
        CHECK(has_code(diags, "import.decode_failed"));

        diags.clear();
        CHECK(!parse_png(importtest::make_png(0, 4, 6), info, diags)); // zero dimension
        CHECK(has_code(diags, "import.source_malformed"));
    }

    // import() surfaces a bad source as ok=false + diagnostics + no artifacts (total, never throws).
    {
        const std::string bad = "nope";
        ImportInput in;
        in.source_path = "bad.png";
        in.source_bytes = bad;
        in.settings = settings;
        in.platform = win;
        const ImportResult r = importer.import(in);
        CHECK(!r.ok);
        CHECK(r.artifacts.empty());
        CHECK(!r.diagnostics.empty());
    }

    IMPORT_TEST_MAIN_END();
}
