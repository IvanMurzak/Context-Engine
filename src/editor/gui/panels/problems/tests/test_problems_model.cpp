// The Problems view model: grouping by file, worst-severity-first ordering, counts, identity
// dedup/promotion, navigation-target display, and inline-marker derivation (R-HUX-005 / R-FILE-003).

#include "context/editor/gui/panels/problems/problems_model.h"

#include "problems_test.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace context::editor::gui::panels::problems;
namespace bridge = context::editor::bridge;

namespace
{

[[nodiscard]] ProblemDiagnostic diag(std::string code, std::string message, Severity sev,
                                     std::string file, std::uint32_t line, std::uint32_t column,
                                     bridge::Stability stability, std::uint64_t generation)
{
    ProblemDiagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.severity = sev;
    d.nav.file = std::move(file);
    d.nav.line = line;
    d.nav.column = column;
    d.stability = stability;
    d.generation = generation;
    return d;
}

} // namespace

int main()
{
    using bridge::Stability;

    // --- Empty set -------------------------------------------------------------------------------
    {
        const ProblemsModel model = build_problems_model({});
        CHECK(model.empty());
        CHECK(model.total == 0);
        CHECK(model.groups.empty());
        CHECK(build_inline_markers({}).empty());
    }

    // --- Grouping + worst-severity-first + counts + inline markers -------------------------------
    {
        std::vector<ProblemDiagnostic> diags;
        diags.push_back(diag("E-A-1", "unclosed brace", Severity::warning, "a.json", 3, 2,
                             Stability::stable, 1));
        diags.push_back(
            diag("E-A-2", "bad ref", Severity::error, "a.json", 1, 1, Stability::stable, 1));
        diags.push_back(
            diag("E-B-1", "hint here", Severity::info, "b.json", 5, 0, Stability::settling, 1));
        // A project-wide diagnostic with no navigable source.
        ProblemDiagnostic pw = diag("E-P-1", "project wide", Severity::hint, "", 0, 0,
                                    Stability::stable, 1);
        diags.push_back(pw);

        const ProblemsModel model = build_problems_model(diags);

        // Three groups, in first-seen file order: a.json, b.json, then the no-file group.
        CHECK(model.groups.size() == 3);
        CHECK(model.groups[0].file == "a.json");
        CHECK(model.groups[1].file == "b.json");
        CHECK(model.groups[2].file.empty());

        // a.json is worst-first: the error precedes the warning even though the warning came first.
        CHECK(model.groups[0].diagnostics.size() == 2);
        CHECK(model.groups[0].diagnostics[0].severity == Severity::error);
        CHECK(model.groups[0].diagnostics[1].severity == Severity::warning);

        // Counts.
        CHECK(model.total == 4);
        CHECK(model.errors == 1);
        CHECK(model.warnings == 1);
        CHECK(model.infos == 1);
        CHECK(model.hints == 1);
        CHECK(model.provisional == 1); // only the b.json settling diagnostic

        // Inline markers: one per NAVIGABLE diagnostic, in grouped order (the no-file hint is excluded).
        const std::vector<InlineMarker> markers = build_inline_markers(diags);
        CHECK(markers.size() == 3);
        CHECK(markers[0].file == "a.json");
        CHECK(markers[0].severity == Severity::error);
        CHECK(!markers[0].provisional);
        CHECK(markers[2].file == "b.json");
        CHECK(markers[2].provisional); // the settling one is drawn provisional
    }

    // --- Navigation-target display ---------------------------------------------------------------
    {
        NavTarget file_line_col;
        file_line_col.file = "scene.json";
        file_line_col.line = 12;
        file_line_col.column = 4;
        CHECK(file_line_col.display() == "scene.json:12:4");
        CHECK(file_line_col.navigable());

        NavTarget file_line;
        file_line.file = "scene.json";
        file_line.line = 12;
        CHECK(file_line.display() == "scene.json:12");

        NavTarget file_only;
        file_only.file = "scene.json";
        CHECK(file_only.display() == "scene.json");

        NavTarget file_pointer;
        file_pointer.file = "scene.json";
        file_pointer.pointer = "/entities/0/name";
        CHECK(file_pointer.display() == "scene.json#/entities/0/name");

        NavTarget none;
        CHECK(!none.navigable());
        CHECK(none.display().empty());
    }

    // --- Identity dedup / promotion (last wins) --------------------------------------------------
    {
        // The SAME diagnostic emitted provisional then stable is ONE problem, promoted (not doubled).
        std::vector<ProblemDiagnostic> diags;
        diags.push_back(
            diag("E-A-2", "bad ref", Severity::error, "a.json", 1, 1, Stability::settling, 1));
        diags.push_back(
            diag("E-A-2", "bad ref", Severity::error, "a.json", 1, 1, Stability::stable, 1));

        const ProblemsModel model = build_problems_model(diags);
        CHECK(model.total == 1);
        CHECK(model.provisional == 0); // the later, stable emission wins
    }

    // --- Explicit key overrides the composite identity -------------------------------------------
    {
        ProblemDiagnostic a = diag("C1", "first text", Severity::error, "x.json", 1, 1,
                                   Stability::stable, 1);
        a.key = "shared";
        ProblemDiagnostic b = diag("C2", "different text", Severity::warning, "y.json", 9, 9,
                                   Stability::stable, 1);
        b.key = "shared"; // same explicit key -> same problem despite different content

        CHECK(a.identity() == "shared");
        const ProblemsModel model = build_problems_model({a, b});
        CHECK(model.total == 1); // collapsed by key
    }

    PROBLEMS_TEST_MAIN_END();
}
