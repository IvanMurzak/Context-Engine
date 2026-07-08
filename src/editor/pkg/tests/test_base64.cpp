// Standard base64 (RFC 4648): encode vectors, decode roundtrip, and STRICT (fail-closed) rejection.

#include "context/editor/pkg/base64.h"
#include "pkg_test.h"

#include <string>

using namespace context::editor::pkg;

int main()
{
    // RFC 4648 §10 test vectors.
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foob") == "Zm9vYg==");
    CHECK(base64_encode("fooba") == "Zm9vYmE=");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");

    // Decode is the inverse.
    CHECK(base64_decode("Zg==").value() == "f");
    CHECK(base64_decode("Zm8=").value() == "fo");
    CHECK(base64_decode("Zm9v").value() == "foo");
    CHECK(base64_decode("Zm9vYmFy").value() == "foobar");
    CHECK(base64_decode("").value() == "");

    // Roundtrip over raw bytes including NUL/high bytes.
    const std::string raw("\x00\x01\x02\xfd\xfe\xff binary", 13);
    CHECK(base64_decode(base64_encode(raw)).value() == raw);

    // Strict rejection (fail-closed): wrong length, invalid char, misplaced padding.
    CHECK(!base64_decode("Zg=").has_value());   // length not a multiple of 4
    CHECK(!base64_decode("Zg=A").has_value());  // pad followed by data
    CHECK(!base64_decode("Z===").has_value());  // pad in the c1 slot
    CHECK(!base64_decode("Zm9*").has_value());  // '*' is not an alphabet char
    CHECK(!base64_decode("Zm 9v").has_value()); // whitespace is not tolerated
    CHECK(!base64_decode("=Zm9").has_value());  // leading pad

    PKG_TEST_MAIN_END();
}
