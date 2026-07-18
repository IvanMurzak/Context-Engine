// The getting-started references point at REAL, committed R-QA-006 samples (R-HUX-010): every
// SampleRef.path must resolve to an existing directory under samples/. This makes the in-editor
// getting-started links rot-if-drift — a removed or renamed sample reddens the build instead of
// shipping a dangling link. Offline by construction: the samples are opened locally, never fetched.

#include "context/editor/gui/help/help_model.h"

#include "help_test.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace context::editor::gui::help;

#ifndef CONTEXT_HELP_SAMPLES_ROOT
#error "CONTEXT_HELP_SAMPLES_ROOT (the repo root holding samples/) must be defined by CMake"
#endif

int main()
{
    const std::vector<SampleRef> samples = getting_started_samples();
    CHECK(!samples.empty());

    const std::filesystem::path root{CONTEXT_HELP_SAMPLES_ROOT};

    for (const SampleRef& s : samples)
    {
        CHECK(!s.id.empty());
        CHECK(!s.title.empty());
        CHECK(!s.summary.empty());
        // Every reference is a project-relative corpus path (offline, under samples/).
        CHECK(s.path.rfind("samples/", 0) == 0);
        CHECK(s.path.find("://") == std::string::npos); // never a URL — no network fetch

        // The referenced sample directory must actually exist in the committed corpus.
        const std::filesystem::path dir = root / s.path;
        if (!std::filesystem::is_directory(dir))
        {
            std::fprintf(stderr,
                         "gui-help-getting-started: getting-started sample %s points at %s which is "
                         "not a directory (fix help::getting_started_samples() or restore the sample)\n",
                         s.id.c_str(), dir.string().c_str());
        }
        CHECK(std::filesystem::is_directory(dir));
    }

    HELP_TEST_MAIN_END();
}
