// Scope-enforcement unit tests (R-SEC-007): the vocabulary, the method→required-scope table, the
// authorize() predicate, parsing, and the launch-ceiling intersection.

#include "context/editor/bridge/scope.h"

#include "bridge_test.h"

using namespace context::editor::bridge;

int main()
{
    // --- baseline: default set is read/query only -----------------------------------------------
    {
        ScopeSet base;
        CHECK(base.has(Scope::read_query)); // baseline is always held
        CHECK(!base.has(Scope::file_write));
        CHECK(!base.has(Scope::session_control));
        CHECK(!base.has(Scope::build_install));
        // names() always leads with read-query and lists no higher scopes.
        const auto names = base.names();
        CHECK(names.size() == 1);
        CHECK(names[0] == "read-query");
    }

    // --- required-scope table -------------------------------------------------------------------
    CHECK(required_scope_for("describe") == Scope::read_query);
    CHECK(required_scope_for("set") == Scope::file_write);
    CHECK(required_scope_for("new") == Scope::file_write);
    CHECK(required_scope_for("package.add") == Scope::build_install);
    CHECK(required_scope_for("build") == Scope::build_install);
    CHECK(required_scope_for("session.play") == Scope::session_control);
    CHECK(required_scope_for("totally.unknown") == Scope::read_query); // unknowns are read/query

    // --- authorize(): a read/query token -------------------------------------------------------
    {
        const ScopeSet read = ScopeSet::read_query();
        CHECK(authorize("describe", read));       // baseline always allowed
        CHECK(!authorize("package.add", read));   // install rejected (FAILURE PATH — the R-SEC-007 gate)
        CHECK(!authorize("build", read));         // build rejected
        CHECK(!authorize("set", read));           // file-write rejected
        CHECK(!authorize("session.play", read));  // session-control rejected
    }

    // --- authorize(): a fully-privileged token --------------------------------------------------
    {
        const ScopeSet all = ScopeSet::all();
        CHECK(authorize("describe", all));
        CHECK(authorize("package.add", all));
        CHECK(authorize("build", all));
        CHECK(authorize("set", all));
        CHECK(authorize("session.play", all));
        CHECK(all.has(Scope::file_write) && all.has(Scope::session_control) &&
              all.has(Scope::build_install));
    }

    // --- parse(): tokens, aliases, unknowns (least privilege) -----------------------------------
    {
        CHECK(!ScopeSet::parse("read").has(Scope::file_write));      // read alias = baseline
        CHECK(ScopeSet::parse("write").has(Scope::file_write));
        CHECK(ScopeSet::parse("file-write").has(Scope::file_write)); // alias
        CHECK(ScopeSet::parse("build").has(Scope::build_install));
        CHECK(ScopeSet::parse("install").has(Scope::build_install)); // alias
        CHECK(ScopeSet::parse("session").has(Scope::session_control));

        const ScopeSet multi = ScopeSet::parse("write, build");
        CHECK(multi.has(Scope::file_write));
        CHECK(multi.has(Scope::build_install));
        CHECK(!multi.has(Scope::session_control));

        // Unknown tokens are ignored — the result stays at the read/query baseline.
        const ScopeSet junk = ScopeSet::parse("root admin superuser");
        CHECK(!junk.has(Scope::file_write));
        CHECK(!junk.has(Scope::build_install));
    }

    // --- intersect(): the launch-time operator ceiling ------------------------------------------
    {
        const ScopeSet ceiling = ScopeSet::parse("write");        // operator allows only file-write
        const ScopeSet requested = ScopeSet::parse("write build"); // client wants file-write + build
        const ScopeSet granted = ceiling.intersect(requested);
        CHECK(granted.has(Scope::file_write));      // survives — in both
        CHECK(!granted.has(Scope::build_install));  // clamped away by the ceiling
        // Clamped token is still rejected for install even though the client asked for it.
        CHECK(!authorize("package.add", granted));
        CHECK(authorize("set", granted));
    }

    // The scope-denied code is a stable, non-empty grep-able string.
    CHECK(std::string(kScopeDeniedCode) == "scope.denied");

    BRIDGE_TEST_MAIN_END();
}
