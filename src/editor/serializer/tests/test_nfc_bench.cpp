// NFC cost measurement (R-FILE-001 spike-mandated: "NFC normalization cost is unmeasured as of
// M0 — the M1 serializer MUST measure it and implement an ASCII quick-check fast path"). Prints
// the measured throughput split — ASCII fast path vs quick-check scan vs full normalization — so
// the numbers land in the module README + the PR. Assertions are deliberately WEAK sanity bounds
// (correctness + "the fast path is not slower than full normalization"), never perf gates: shared
// CI runners are not perf-isolated (R-QA-009 discipline; the perf floors live in bench/).

#include "context/editor/serializer/nfc.h"

#include "serializer_test.h"

#include <chrono>
#include <cstdio>
#include <string>

using context::editor::serializer::is_ascii;
using context::editor::serializer::is_nfc_quick;
using context::editor::serializer::normalize_nfc;

namespace
{

using Clock = std::chrono::steady_clock;

double mb_per_s(std::size_t bytes, std::chrono::nanoseconds elapsed)
{
    const double seconds = static_cast<double>(elapsed.count()) / 1e9;
    return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
}

} // namespace

int main()
{
    // ~4 MB corpora, shaped like authored content.
    std::string ascii_text;
    while (ascii_text.size() < 4u * 1024 * 1024)
        ascii_text += "{\"entity\": 42, \"name\": \"MainCamera\", \"transform\": [0, 1, -5]} ";

    std::string nfc_text; // non-ASCII but already NFC (composed é + CJK)
    while (nfc_text.size() < 4u * 1024 * 1024)
        nfc_text += "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E na\xC3\xAFve ";

    std::string decomposed_text; // NOT NFC: every é is e + U+0301
    while (decomposed_text.size() < 4u * 1024 * 1024)
        decomposed_text += "cafe\xCC\x81 re\xCC\x81sume\xCC\x81 e\xCC\x81tude ";

    constexpr int kReps = 8;

    // 1) ASCII fast path: the is_ascii scan is the entire cost for ASCII content.
    Clock::duration ascii_scan{};
    bool all_ascii = true;
    for (int i = 0; i < kReps; ++i)
    {
        const auto t0 = Clock::now();
        all_ascii = all_ascii && is_ascii(ascii_text);
        ascii_scan += Clock::now() - t0;
    }
    CHECK(all_ascii);

    // 2) Quick-check scan on NFC non-ASCII content (decode + table probes, no rewrite).
    Clock::duration quick_scan{};
    bool all_nfc = true;
    for (int i = 0; i < kReps; ++i)
    {
        const auto t0 = Clock::now();
        all_nfc = all_nfc && is_nfc_quick(nfc_text);
        quick_scan += Clock::now() - t0;
    }
    CHECK(all_nfc);

    // 3) normalize_nfc on already-NFC content: quick check + nullopt (the no-allocation path).
    Clock::duration normalize_noop{};
    for (int i = 0; i < kReps; ++i)
    {
        const auto t0 = Clock::now();
        const auto result = normalize_nfc(nfc_text);
        normalize_noop += Clock::now() - t0;
        CHECK(!result.has_value());
    }

    // 4) FULL normalization on decomposed content (decompose + reorder + compose + re-encode).
    Clock::duration full_normalize{};
    std::string normalized;
    for (int i = 0; i < kReps; ++i)
    {
        const auto t0 = Clock::now();
        auto result = normalize_nfc(decomposed_text);
        full_normalize += Clock::now() - t0;
        CHECK(result.has_value());
        if (result.has_value())
            normalized = std::move(*result);
    }
    CHECK(is_nfc_quick(normalized));
    CHECK(normalized.size() < decomposed_text.size()); // composition shrank the bytes

    const std::size_t n = ascii_text.size() * kReps;
    const std::size_t n_nfc = nfc_text.size() * kReps;
    const std::size_t n_dec = decomposed_text.size() * kReps;
    std::printf("nfc-bench (R-FILE-001 measurement, %d x ~4 MiB, steady_clock):\n", kReps);
    std::printf("  ascii fast path (is_ascii scan)   : %9.1f MB/s\n", mb_per_s(n, ascii_scan));
    std::printf("  quick-check scan (NFC non-ASCII)  : %9.1f MB/s\n",
                mb_per_s(n_nfc, quick_scan));
    std::printf("  normalize_nfc no-op (already NFC) : %9.1f MB/s\n",
                mb_per_s(n_nfc, normalize_noop));
    std::printf("  full NFC normalization (decomposed): %8.1f MB/s\n",
                mb_per_s(n_dec, full_normalize));

    // Weak sanity assertions only (see the header note): the ASCII fast path must not be slower
    // than full normalization of the same byte volume, and every measured pass moved real bytes.
    CHECK(ascii_scan.count() > 0);
    CHECK(full_normalize.count() > 0);
    CHECK(mb_per_s(n, ascii_scan) > mb_per_s(n_dec, full_normalize));

    SERIALIZER_TEST_MAIN_END();
}
