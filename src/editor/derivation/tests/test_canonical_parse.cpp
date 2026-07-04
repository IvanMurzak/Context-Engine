// Seam tests for the canonical parse node, now backed by the REAL canonical-JSON serializer
// (M2, issue #42): JSON canonicalizes fully; non-JSON passes through raw (raw ≡ canonical — the
// binary-sidecar rule); the node stays deterministic and total (R-FILE-001, L-22).

#include "context/editor/derivation/canonical_parse.h"

#include "derivation_test.h"

#include <string>

using context::editor::derivation::canonical_hash_of;
using context::editor::derivation::canonical_parse;

int main()
{
    // Happy path: JSON input yields THE canonical form — sorted keys, 2-space indent, trailing LF.
    {
        auto form = canonical_parse("{\"b\": 2, \"a\": 1}");
        CHECK(form.is_json);
        CHECK(form.bytes == "{\n  \"a\": 1,\n  \"b\": 2\n}\n");
        CHECK(form.canonical_hash == canonical_hash_of(form.bytes));
        CHECK(form.diagnostics.empty());
    }

    // Formatting-insensitivity is the whole point (raw-byte != canonical, R-FILE-001): whitespace,
    // key order, and number notation differences must canonicalize to ONE hash so downstream nodes
    // memoize past a no-op edit.
    {
        auto a = canonical_parse("{\"x\": 100, \"y\": 0.5}");
        auto b = canonical_parse("  {\n\t\"y\" : 5e-1 ,\n\t\"x\" : 1e2\n}  ");
        auto c = canonical_parse("{\"y\":0.50,\"x\":100.0}");
        CHECK(a.is_json);
        CHECK(a.canonical_hash == b.canonical_hash);
        CHECK(b.canonical_hash == c.canonical_hash);
        CHECK(a.bytes == c.bytes);
    }

    // Genuinely different content yields a different canonical hash.
    {
        CHECK(canonical_parse("{\"a\": 1}").canonical_hash !=
              canonical_parse("{\"a\": 2}").canonical_hash);
    }

    // Fixpoint: canonicalizing canonical output is a byte-identical no-op.
    {
        auto once = canonical_parse("{\"z\": [1.0, 2e0], \"a\": \"e\\u0301\"}");
        auto twice = canonical_parse(once.bytes);
        CHECK(once.is_json);
        CHECK(twice.bytes == once.bytes);
        CHECK(twice.canonical_hash == once.canonical_hash);
    }

    // Non-JSON input (binary sidecars, TS/shader text — the L-32 carve-outs): NO canonicalization
    // pass. The canonical form IS the raw bytes, so the canonical hash equals the raw-byte hash by
    // construction, and different raw bytes — even whitespace-only differences — stay different.
    {
        auto bin = canonical_parse("not json at all");
        CHECK(!bin.is_json);
        CHECK(bin.bytes == "not json at all");
        CHECK(bin.canonical_hash == canonical_hash_of("not json at all"));
        CHECK(!bin.diagnostics.empty()); // the parse failure travels with the form

        CHECK(canonical_parse("a b").canonical_hash != canonical_parse("a  b").canonical_hash);
    }

    // Edge: empty input is total and yields the empty non-JSON form with the empty-string hash.
    {
        auto empty = canonical_parse("");
        CHECK(!empty.is_json);
        CHECK(empty.bytes.empty());
        CHECK(empty.canonical_hash == canonical_hash_of(""));
    }

    // Encoding heals surface as machine-readable diagnostics while the bytes come back clean
    // (BOM stripped, CRLF -> LF), R-FILE-003.
    {
        auto healed = canonical_parse("\xEF\xBB\xBF{\"a\": 1}\r\n");
        CHECK(healed.is_json);
        CHECK(healed.bytes == "{\n  \"a\": 1\n}\n");
        bool bom = false;
        bool crlf = false;
        for (const auto& d : healed.diagnostics)
        {
            bom = bom || d.code == "encoding.bom";
            crlf = crlf || d.code == "encoding.crlf";
        }
        CHECK(bom);
        CHECK(crlf);
    }

    // Determinism: the same input hashes identically across calls.
    {
        CHECK(canonical_parse("{\"k\": true}").canonical_hash ==
              canonical_parse("{\"k\": true}").canonical_hash);
        CHECK(canonical_hash_of("determinism") == canonical_hash_of("determinism"));
    }

    DERIVATION_TEST_MAIN_END();
}
