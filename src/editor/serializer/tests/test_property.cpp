// Property tests (R-FILE-001, R-QA-013): serialize∘parse idempotence and the re-canonicalization
// fixpoint over seeded random documents — the M0 parse-bench fixpoint method (3,810 files,
// 0 mismatches) carried in-repo as a deterministic generator instead of an external corpus.

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "serializer_test.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using context::editor::serializer::canonicalize;
using context::editor::serializer::JsonMember;
using context::editor::serializer::JsonValue;
using context::editor::serializer::parse_json;
using context::editor::serializer::serialize_canonical;

namespace
{

// Deterministic xorshift64* — the seeds pin the whole corpus, so every run (and every CI leg)
// exercises the identical documents.
struct Rng
{
    std::uint64_t state;

    std::uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }

    std::uint32_t below(std::uint32_t bound) { return static_cast<std::uint32_t>(next() % bound); }
};

std::string random_string(Rng& rng)
{
    static const char* kUnicodePool[] = {
        "\xC3\xA9",         // é (already NFC)
        "e\xCC\x81",        // e + combining acute (NOT NFC — the writer must normalize)
        "\xE6\x97\xA5",     // 日
        "\xE1\x84\x80\xE1\x85\xA1", // Hangul jamo pair (NOT NFC)
        "\xF0\x9F\x98\x80", // U+1F600 (astral)
        "\xE2\x84\xAB",     // ANGSTROM SIGN (NOT NFC — singleton)
    };
    std::string s;
    const std::uint32_t len = rng.below(12);
    for (std::uint32_t i = 0; i < len; ++i)
    {
        const std::uint32_t pick = rng.below(24);
        if (pick < 16)
            s.push_back(static_cast<char>('a' + rng.below(26)));
        else if (pick < 18)
            s.push_back(static_cast<char>(rng.below(0x20))); // control char (writer \u-escapes)
        else if (pick < 20)
            s.push_back(rng.below(2) == 0 ? '"' : '\\');
        else
            s += kUnicodePool[rng.below(6)];
    }
    return s;
}

double random_double(Rng& rng)
{
    static const double kEdges[] = {0.0,   -0.0,  0.1,    1.0 / 3.0, 1e20,   1e21,
                                    1e-6,  1e-7,  5e-324, 1.7976931348623157e308,
                                    -2.5,  100.0, 9007199254740993.0};
    if (rng.below(3) == 0)
        return kEdges[rng.below(13)];
    while (true)
    {
        const std::uint64_t bits = rng.next();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        if (v == v && v != std::numeric_limits<double>::infinity() &&
            v != -std::numeric_limits<double>::infinity())
            return v;
    }
}

JsonValue random_value(Rng& rng, int depth)
{
    JsonValue v;
    const std::uint32_t pick = rng.below(depth >= 4 ? 6u : 8u); // cap nesting depth
    switch (pick)
    {
    case 0:
        v.type = JsonValue::Type::null_value;
        return v;
    case 1:
        v.type = JsonValue::Type::boolean;
        v.boolean_value = rng.below(2) == 0;
        return v;
    case 2:
        v.type = JsonValue::Type::integer;
        v.int_value = static_cast<std::int64_t>(rng.next());
        return v;
    case 3:
        v.type = JsonValue::Type::unsigned_integer;
        v.uint_value = rng.next() | (1ULL << 63); // top bit forced: always in the u64-only domain
        return v;
    case 4:
        v.type = JsonValue::Type::number;
        v.number_value = random_double(rng);
        return v;
    case 5:
        v.type = JsonValue::Type::string;
        v.string_value = random_string(rng);
        return v;
    case 6:
    {
        v.type = JsonValue::Type::array;
        const std::uint32_t n = rng.below(5);
        for (std::uint32_t i = 0; i < n; ++i)
            v.elements.push_back(random_value(rng, depth + 1));
        return v;
    }
    default:
    {
        v.type = JsonValue::Type::object;
        const std::uint32_t n = rng.below(5);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            JsonMember m;
            // Unique keys by construction (the parser rejects duplicates).
            m.key = random_string(rng) + "_" + std::to_string(i);
            m.value = random_value(rng, depth + 1);
            v.members.push_back(std::move(m));
        }
        return v;
    }
    }
}

// Inject random inter-token whitespace into canonical bytes WITHOUT touching string contents —
// simulates external formatting noise the canonicalizer must erase (L-22 memoization).
std::string add_formatting_noise(const std::string& canonical, Rng& rng)
{
    std::string noisy;
    noisy.reserve(canonical.size() * 2);
    bool in_string = false;
    bool escaped = false;
    static const char* kNoise[] = {" ", "  ", "\t", "\n", ""};
    for (char c : canonical)
    {
        if (in_string)
        {
            noisy.push_back(c);
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
        {
            noisy.push_back(c);
            in_string = true;
            continue;
        }
        if (c == '{' || c == '[' || c == ',' || c == ':')
        {
            noisy.push_back(c);
            noisy += kNoise[rng.below(5)];
            continue;
        }
        if (c == '}' || c == ']')
        {
            noisy += kNoise[rng.below(5)];
            noisy.push_back(c);
            continue;
        }
        noisy.push_back(c);
    }
    return noisy;
}

} // namespace

int main()
{
    Rng rng{20260704ULL}; // the corpus seed — deterministic across runs and platforms

    int documents = 0;
    for (int i = 0; i < 300; ++i)
    {
        const JsonValue tree = random_value(rng, 0);

        // Property 1 — serialize∘parse idempotence: writing a parsed canonical document
        // reproduces the identical bytes.
        std::string first;
        CHECK(serialize_canonical(tree, first));
        const auto reparsed = parse_json(first);
        CHECK(reparsed.ok);
        std::string second;
        CHECK(serialize_canonical(reparsed.root, second));
        CHECK(first == second);

        // Property 2 — re-canonicalization fixpoint: canonicalizing canonical bytes is a no-op.
        const auto once = canonicalize(first);
        CHECK(once.is_json);
        CHECK(once.bytes == first);

        // Property 3 — formatting noise erases: noisy variants canonicalize to the same bytes
        // and the same canonical hash (downstream derivation memoizes past no-op edits).
        const std::string noisy = add_formatting_noise(first, rng);
        const auto from_noisy = canonicalize(noisy);
        CHECK(from_noisy.is_json);
        CHECK(from_noisy.bytes == first);
        CHECK(from_noisy.canonical_hash == once.canonical_hash);

        ++documents;
    }
    CHECK(documents == 300);

    SERIALIZER_TEST_MAIN_END();
}
