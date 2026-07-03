// Single-instance lock unit tests (R-BRIDGE-001 / R-ARCH-005): acquire, the second-instance attach
// signal, release-then-reacquire, shared/exclusive compatibility, and canonical-path aliasing.

#include "context/editor/bridge/lock.h"

#include "bridge_test.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using namespace context::editor::bridge;

namespace
{
fs::path make_temp_project()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-bridge-lock-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

int main()
{
    const fs::path project = make_temp_project();

    // --- happy path: a write-capable instance acquires the exclusive lock -----------------------
    {
        ProjectLock lock(project);
        CHECK(lock.try_acquire(LockMode::exclusive) == LockOutcome::acquired);
        CHECK(lock.held());
        CHECK(lock.mode() == LockMode::exclusive);
        // The lock file materialized under <project>/.editor/lock.
        std::error_code ec;
        CHECK(fs::exists(lock.lock_path(), ec));

        // --- FAILURE/attach path: a 2nd write-capable instantiation detects the live lock --------
        {
            ProjectLock second(project);
            CHECK(second.try_acquire(LockMode::exclusive) == LockOutcome::already_held);
            CHECK(!second.held()); // the attach signal — do not boot a second instance
        }

        // A shared request also conflicts with a held exclusive lock.
        {
            ProjectLock reader(project);
            CHECK(reader.try_acquire(LockMode::shared) == LockOutcome::already_held);
        }
    } // lock released here (RAII)

    // --- release-then-reacquire: the lock is free again -----------------------------------------
    {
        ProjectLock relock(project);
        CHECK(relock.try_acquire(LockMode::exclusive) == LockOutcome::acquired);
    }

    // --- shared/shared coexist; shared then exclusive conflicts ---------------------------------
    {
        ProjectLock r1(project);
        ProjectLock r2(project);
        CHECK(r1.try_acquire(LockMode::shared) == LockOutcome::acquired);
        CHECK(r2.try_acquire(LockMode::shared) == LockOutcome::acquired); // two readers OK
        ProjectLock writer(project);
        CHECK(writer.try_acquire(LockMode::exclusive) == LockOutcome::already_held); // reader blocks
    }

    // --- canonical-path aliasing: two spellings of the same dir exclude each other ---------------
    // R-BRIDGE-001: exclusion holds regardless of how the path is spelled (junction/subst/UNC/".").
    {
        ProjectLock canonical(project);
        CHECK(canonical.try_acquire(LockMode::exclusive) == LockOutcome::acquired);

        const fs::path aliased = project / "." / ".." / project.filename();
        ProjectLock spelled_differently(aliased);
        CHECK(spelled_differently.canonical_key() == canonical.canonical_key());
        CHECK(spelled_differently.try_acquire(LockMode::exclusive) == LockOutcome::already_held);
    }

    // Best-effort cleanup (a leftover temp dir must not fail the test).
    std::error_code ec;
    fs::remove_all(project, ec);

    BRIDGE_TEST_MAIN_END();
}
