// SHA-256 + HMAC-SHA256 against published test vectors (FIPS 180-4, RFC 4231).

#include "context/editor/filesync/hmac.h"
#include "context/editor/filesync/sha256.h"
#include "filesync_test.h"

#include <string>

using namespace context::editor::filesync;

int main()
{
    // FIPS 180-4 known-answer vectors.
    CHECK(to_hex(sha256("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(to_hex(sha256("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(sha256("The quick brown fox jumps over the lazy dog")) ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");

    // Determinism.
    CHECK(sha256("abc") == sha256("abc"));
    CHECK(sha256("abc") != sha256("abd"));

    // RFC 4231 HMAC-SHA256 Test Case 2: key "Jefe".
    CHECK(hmac_sha256_hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // A different key over the same message yields a different MAC (foreign-log-replay basis).
    CHECK(hmac_sha256_hex("keyA", "message") != hmac_sha256_hex("keyB", "message"));

    // RFC 4231 HMAC-SHA256 Test Case 6: a key LONGER than the 64-byte block is hashed down first
    // (RFC 2104 key reduction). A published known-answer vector pins that branch, rather than only
    // asserting the output length.
    const std::string long_key(131, static_cast<char>(0xaa));
    CHECK(hmac_sha256_hex(long_key, "Test Using Larger Than Block-Size Key - Hash Key First") ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    FILESYNC_TEST_MAIN_END();
}
