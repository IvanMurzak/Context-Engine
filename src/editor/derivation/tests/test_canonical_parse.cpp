// Unit tests for the minimal M1 canonical-parse placeholder (canonical_parse.h).

#include "context/editor/derivation/canonical_parse.h"

#include "derivation_test.h"

using context::editor::derivation::canonical_hash_of;
using context::editor::derivation::canonical_parse;

int main()
{
    // Happy path: internal whitespace collapses, leading/trailing trims.
    {
        auto form = canonical_parse("  hello   world  ");
        CHECK(form.bytes == "hello world");
        CHECK(form.canonical_hash == canonical_hash_of("hello world"));
    }

    // Whitespace-insensitivity is the whole point (raw-byte != canonical, R-FILE-001): three
    // formatting-different inputs must canonicalize to ONE hash so downstream nodes memoize.
    {
        auto a = canonical_parse("a b");
        auto b = canonical_parse("  a\tb\n");
        auto c = canonical_parse("a\n\n\n   b");
        CHECK(a.canonical_hash == b.canonical_hash);
        CHECK(b.canonical_hash == c.canonical_hash);
        CHECK(a.bytes == "a b");
        CHECK(c.bytes == "a b");
    }

    // Genuinely different content yields a different canonical hash.
    {
        CHECK(canonical_parse("a b").canonical_hash != canonical_parse("a c").canonical_hash);
    }

    // Edge: empty / whitespace-only input is total and yields the empty-string canonical form.
    {
        auto empty = canonical_parse("");
        auto blank = canonical_parse("   \t\n  ");
        CHECK(empty.bytes.empty());
        CHECK(blank.bytes.empty());
        CHECK(empty.canonical_hash == blank.canonical_hash);
        CHECK(empty.canonical_hash == canonical_hash_of(""));
    }

    // Determinism: the same input hashes identically across calls.
    {
        CHECK(canonical_hash_of("determinism") == canonical_hash_of("determinism"));
    }

    DERIVATION_TEST_MAIN_END();
}
