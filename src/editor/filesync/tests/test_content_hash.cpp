// content_hash: determinism, sensitivity, empty input (R-QA-013 happy + edge).

#include "context/editor/filesync/content_hash.h"
#include "filesync_test.h"

using namespace context::editor::filesync;

int main()
{
    // Deterministic: same bytes -> same hash.
    CHECK(content_hash("hello world") == content_hash("hello world"));

    // Sensitive: different bytes -> different hash (overwhelmingly likely; these differ).
    CHECK(content_hash("hello world") != content_hash("hello worlds"));
    CHECK(content_hash("a") != content_hash("b"));

    // A one-bit / one-char flip changes the hash.
    CHECK(content_hash("scene:{x:1}") != content_hash("scene:{x:2}"));

    // Empty input is well-defined and stable (the FNV-1a offset basis) and differs from a NUL byte.
    CHECK(content_hash("") == content_hash(""));
    CHECK(content_hash("") != content_hash(std::string_view{"\0", 1}));

    FILESYNC_TEST_MAIN_END();
}
