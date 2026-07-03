// Path normalization + project-root jail (R-SEC-008).

#include "context/editor/filesync/path_jail.h"
#include "filesync_test.h"

using namespace context::editor::filesync;

int main()
{
    // --- normalize_path -------------------------------------------------------------------------
    CHECK(normalize_path("proj/a.txt") == "proj/a.txt");
    CHECK(normalize_path("proj\\sub\\a.txt") == "proj/sub/a.txt"); // backslashes unified
    CHECK(normalize_path("proj/./a.txt") == "proj/a.txt");         // '.' collapsed
    CHECK(normalize_path("proj/sub/../a.txt") == "proj/a.txt");    // '..' resolved
    CHECK(normalize_path("proj//a.txt") == "proj/a.txt");          // empty segment collapsed
    CHECK(normalize_path("proj/sub/") == "proj/sub");              // trailing slash dropped
    CHECK(normalize_path("proj/../../etc") == "../etc");           // relative may ascend

    // --- inside the jail ------------------------------------------------------------------------
    CHECK(is_inside_jail("proj", "proj/a.txt"));
    CHECK(is_inside_jail("proj", "proj/sub/deep/a.txt"));
    CHECK(is_inside_jail("proj", "proj"));               // the root itself
    CHECK(is_inside_jail("proj", "proj/sub/../a.txt"));  // normalizes back inside

    // --- escaping the jail is refused -----------------------------------------------------------
    CHECK(!is_inside_jail("proj", "proj/../etc/passwd")); // traversal out
    CHECK(!is_inside_jail("proj", "../evil.txt"));        // relative escape
    CHECK(!is_inside_jail("proj", "/etc/passwd"));        // absolute escape
    CHECK(!is_inside_jail("proj", "project2/a.txt"));     // sibling with shared prefix
    CHECK(!is_inside_jail("proj", "proj/../proj-evil"));  // prefix-adjacent escape

    FILESYNC_TEST_MAIN_END();
}
