// Import-settings resolution + the R-FILE-010 settings-hash component.

#include "context/editor/import/import_settings.h"

#include "import_test.h"

using namespace context::editor::import;

int main()
{
    // Empty / absent meta -> defaults + the canonical empty-object bytes + a stable hash.
    {
        const ImportSettings s = resolve_import_settings("", "windows");
        CHECK(s.srgb);
        CHECK(s.generate_mipmaps);
        CHECK(s.canonical_bytes == "{}\n");
        CHECK(s.hash != 0);
        // Malformed meta is total too (defaults, not a throw).
        const ImportSettings bad = resolve_import_settings("not json <<<", "windows");
        CHECK(bad.canonical_bytes == "{}\n");
        CHECK(bad.hash == s.hash);
    }

    // importSettings knobs are read + hashed.
    {
        const std::string meta =
            "{\"importSettings\":{\"srgb\":false,\"generateMipmaps\":false}}";
        const ImportSettings s = resolve_import_settings(meta, "windows");
        CHECK(!s.srgb);
        CHECK(!s.generate_mipmaps);
        // Different settings -> different key component (the R-FILE-010 soundness bit).
        CHECK(s.hash != resolve_import_settings("", "windows").hash);
    }

    // Equivalent authorings (key order) canonicalize to the SAME hash (R-FILE-001 fixpoint).
    {
        const ImportSettings a =
            resolve_import_settings("{\"importSettings\":{\"srgb\":false,\"generateMipmaps\":true}}",
                                    "windows");
        const ImportSettings b =
            resolve_import_settings("{\"importSettings\":{\"generateMipmaps\":true,\"srgb\":false}}",
                                    "windows");
        CHECK(a.hash == b.hash);
        CHECK(a.canonical_bytes == b.canonical_bytes);
    }

    // The reserved platforms.<id> block overrides importSettings for THAT platform only.
    {
        const std::string meta =
            "{\"importSettings\":{\"srgb\":true},\"platforms\":{\"web\":{\"srgb\":false}}}";
        const ImportSettings win = resolve_import_settings(meta, "windows");
        const ImportSettings web = resolve_import_settings(meta, "web");
        CHECK(win.srgb);       // no windows override -> base value
        CHECK(!web.srgb);      // web override applied
        CHECK(win.hash != web.hash); // per-platform settings key apart
    }

    IMPORT_TEST_MAIN_END();
}
