// SHA-512 conformance (FIPS 180-4 known-answer vectors) + determinism.

#include "context/editor/pkg/sha512.h"
#include "pkg_test.h"

#include <string>

using namespace context::editor::pkg;

int main()
{
    // NIST/FIPS-180-4 known-answer vectors.
    CHECK(to_hex(sha512("")) ==
          "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
          "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    CHECK(to_hex(sha512("abc")) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(to_hex(sha512("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
          "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");

    // A single-block boundary (111 bytes -> 1 padded block) and a two-block message (112 -> 2).
    CHECK(to_hex(sha512(std::string(111, 'a'))).size() == 128);
    CHECK(to_hex(sha512(std::string(112, 'a'))).size() == 128);

    // Determinism: same input -> same digest; a one-bit change -> a different digest.
    CHECK(sha512("context-engine") == sha512("context-engine"));
    CHECK(sha512("context-engine") != sha512("context-enginf"));

    // to_hex length is exactly 128 lowercase hex chars.
    const std::string hex = to_hex(sha512("x"));
    CHECK(hex.size() == 128);
    for (char c : hex)
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));

    PKG_TEST_MAIN_END();
}
