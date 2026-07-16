// Golden-pack REBASELINE tool (NOT a ctest). Rewrites tests/corpus/<name>.pack from each committed
// <name>.scenes.json case. Golden pack bytes are platform-independent (little-endian + FNV +
// canonical JSON), so a golden built on any OS matches the 3-OS CI legs. Run this ONLY for an
// intentional, reviewed format change (docs/chunk-pack-format.md) — never to paper over a red gate.

#include "pack_corpus.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main()
{
    const fs::path dir = PACK_CORPUS_DIR;
    const std::vector<fs::path> cases = pack_corpus::list_cases(dir);
    if (cases.empty())
    {
        std::fprintf(stderr, "pack_golden_gen: no <name>.scenes.json cases under %s\n",
                     dir.string().c_str());
        return 1;
    }

    int written = 0;
    for (const fs::path& scenes_file : cases)
    {
        const pack_corpus::CaseResult built = pack_corpus::build_pack_from_case(scenes_file);
        if (!built.ok)
        {
            std::fprintf(stderr, "pack_golden_gen: FAILED to build pack for %s\n",
                         scenes_file.string().c_str());
            return 1;
        }
        const fs::path golden = pack_corpus::golden_path(scenes_file);
        std::ofstream out(golden, std::ios::binary | std::ios::trunc);
        out.write(built.bytes.data(), static_cast<std::streamsize>(built.bytes.size()));
        if (!out.good())
        {
            std::fprintf(stderr, "pack_golden_gen: FAILED to write %s\n", golden.string().c_str());
            return 1;
        }
        std::fprintf(stdout, "wrote %s (%zu bytes)\n", golden.filename().string().c_str(),
                     built.bytes.size());
        ++written;
    }
    std::fprintf(stdout, "pack_golden_gen: regenerated %d golden pack(s)\n", written);
    return 0;
}
