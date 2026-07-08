// ustar reader/writer: write->read roundtrip (files + dirs + a long split path), determinism, and
// fail-closed rejection of a truncated / corrupted archive.

#include "context/editor/pkg/tar.h"
#include "pkg_test.h"

#include <string>
#include <vector>

using namespace context::editor::pkg;

int main()
{
    std::vector<TarEntry> entries = {
        {"package/", "", true},
        {"package/package.json", "{\"name\":\"demo\",\"version\":\"1.0.0\"}", false},
        {"package/index.js", "module.exports = 42;\n", false},
        {"package/lib/util.js", std::string(600, 'x'), false}, // >1 block of data
    };

    const std::optional<std::string> archive = tar_write(entries);
    CHECK(archive.has_value());
    CHECK(archive->size() % 512 == 0); // block-aligned

    // Deterministic: the same entries produce byte-identical archives.
    CHECK(tar_write(entries).value() == *archive);

    const std::optional<std::vector<TarEntry>> read = tar_read(*archive);
    CHECK(read.has_value());
    CHECK(read->size() == entries.size());
    bool saw_index = false, saw_util = false, saw_dir = false;
    for (const TarEntry& e : *read)
    {
        if (e.path == "package/index.js")
        {
            saw_index = true;
            CHECK(e.data == "module.exports = 42;\n");
            CHECK(!e.is_dir);
        }
        if (e.path == "package/lib/util.js")
        {
            saw_util = true;
            CHECK(e.data == std::string(600, 'x'));
        }
        if (e.path == "package/" || e.path == "package")
        {
            saw_dir = true;
            CHECK(e.is_dir);
        }
    }
    CHECK(saw_index);
    CHECK(saw_util);
    CHECK(saw_dir);

    // A long path that splits cleanly into the ustar prefix/name fields roundtrips.
    {
        std::string longdir(120, 'd');
        // Insert a '/' so the split point exists within the first 100 bytes.
        longdir[80] = '/';
        std::vector<TarEntry> deep = {{longdir + "/f.txt", "hi", false}};
        const std::optional<std::string> a = tar_write(deep);
        CHECK(a.has_value());
        const std::optional<std::vector<TarEntry>> r = tar_read(*a);
        CHECK(r.has_value());
        CHECK(r->size() == 1);
        CHECK((*r)[0].path == longdir + "/f.txt");
        CHECK((*r)[0].data == "hi");
    }

    // Fail-closed: a truncated archive whose cut lands INSIDE a declared file body (the header says
    // more bytes than remain). entry[0] is the "package/" dir (no body); entry[1] is package.json,
    // whose data block starts at offset 1024 — cutting at 1030 leaves its declared bytes
    // unsatisfiable, so tar_read must reject rather than partially extract.
    {
        std::string truncated = archive->substr(0, 1030);
        CHECK(!tar_read(truncated).has_value());
    }
    // Fail-closed: a corrupted header (flip a byte in the first header's name) breaks the checksum.
    {
        std::string corrupt = *archive;
        corrupt[2] = corrupt[2] ^ 0x7f;
        CHECK(!tar_read(corrupt).has_value());
    }
    // A sub-header-sized garbage input yields no entries (never a partial extract).
    CHECK(tar_read(std::string(300, 'Z')).has_value());
    CHECK(tar_read(std::string(300, 'Z'))->empty());
    // A full-block of non-ustar garbage IS rejected (bad magic).
    CHECK(!tar_read(std::string(512, 'Z')).has_value());

    PKG_TEST_MAIN_END();
}
