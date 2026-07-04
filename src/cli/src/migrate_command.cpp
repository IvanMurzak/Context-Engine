// `context migrate` implementation (see migrate_command.h).

#include "context/cli/migrate_command.h"

#include "context/editor/contract/json.h"
#include "context/editor/migrate/migrate_document.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace migrate = editor::migrate;
namespace serializer = editor::serializer;
namespace fs = std::filesystem;

namespace
{

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// Atomic-enough in-place rewrite for a one-shot CLI: write the full bytes to a sibling temp file,
// then rename over the original (an atomic replace on POSIX; MoveFileEx-replace on Windows). The
// daemon's fsync-hardened write path (R-FILE-004) is not required here — `context migrate` is an
// explicit user-invoked bulk op, and a live daemon reconciles the rewrite as an external edit.
bool write_file_atomically(const fs::path& path, const std::string& bytes)
{
    const fs::path temp = path.string() + ".ctx-migrate-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec)
    {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

// Collect the candidate files: the target itself when it is a regular file, else every *.json
// under it (recursive), skipping dot-directories (.editor control state, .git, …). Sorted so the
// report (and any failure) is deterministic.
std::vector<fs::path> collect_candidates(const fs::path& target)
{
    std::vector<fs::path> files;
    if (fs::is_regular_file(target))
    {
        files.push_back(target);
        return files;
    }
    std::error_code ec;
    fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec))
        {
            if (!name.empty() && name[0] == '.')
                it.disable_recursion_pending(); // never descend into dot-dirs (.editor, .git)
        }
        else if (entry.is_regular_file(ec) && name.size() > 5 &&
                 name.compare(name.size() - 5, 5, ".json") == 0)
        {
            files.push_back(entry.path());
        }
        it.increment(ec);
    }
    std::sort(files.begin(), files.end());
    return files;
}

Json diagnostics_json(const std::vector<migrate::MigrationDiagnostic>& diagnostics)
{
    Json out = Json::array();
    for (const migrate::MigrationDiagnostic& d : diagnostics)
    {
        Json entry = Json::object();
        entry.set("code", Json(d.code));
        entry.set("message", Json(d.message));
        entry.set("pointer", Json(d.pointer));
        entry.set("blocking", Json(d.blocking));
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace

Envelope run_migrate_with(const std::string& target,
                          const std::map<std::string, std::string>& flags,
                          const migrate::MigrationSet& set)
{
    const bool dry_run = flags.find("dry-run") != flags.end();
    const auto project_it = flags.find("project");
    const std::string root = project_it != flags.end() ? project_it->second : std::string(".");
    const fs::path target_path = target.empty() ? fs::path(root) : fs::path(target);

    std::error_code ec;
    if (!fs::exists(target_path, ec) || ec)
        return Envelope::failure("file.not_found",
                                 "migrate target does not exist: " + target_path.string());

    const std::vector<fs::path> candidates = collect_candidates(target_path);

    Json entries = Json::array();
    std::uint64_t migrated = 0;
    std::uint64_t canonicalized = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t skipped = 0;
    std::uint64_t failed = 0;

    for (const fs::path& file : candidates)
    {
        Json entry = Json::object();
        entry.set("path", Json(file.generic_string()));

        std::string source;
        if (!read_file(file, source))
        {
            entry.set("action", Json("failed"));
            entry.set("error", Json("internal.error: the file could not be read"));
            entries.push_back(std::move(entry));
            ++failed;
            continue;
        }

        // The same canonical parse the tool-save path runs (R-FILE-001): non-JSON content (binary
        // sidecars, TS text — the L-32 carve-outs) has no canonicalization pass and is skipped.
        serializer::CanonicalizeResult canonical = serializer::canonicalize(source);
        if (!canonical.is_json)
        {
            entry.set("action", Json("skipped-non-json"));
            entries.push_back(std::move(entry));
            ++skipped;
            continue;
        }

        migrate::MigrateOptions options;
        options.stamp_registered_sites = true; // the L-37 bulk semantics: stamp what it rewrites
        const migrate::DocumentMigrationResult result =
            migrate::migrate_document(canonical.root, set, options);

        if (!result.ok)
        {
            // Blocking findings: the file is reported and left byte-for-byte untouched.
            entry.set("action", Json("failed"));
            entry.set("diagnostics", diagnostics_json(result.diagnostics));
            entries.push_back(std::move(entry));
            ++failed;
            continue;
        }

        std::string rewritten;
        if (!serializer::serialize_canonical(canonical.root, rewritten))
        {
            entry.set("action", Json("failed"));
            entry.set("error", Json("internal.error: the migrated tree did not serialize"));
            entries.push_back(std::move(entry));
            ++failed;
            continue;
        }

        if (rewritten == source)
        {
            entry.set("action", Json("unchanged"));
            if (!result.diagnostics.empty())
                entry.set("diagnostics", diagnostics_json(result.diagnostics));
            entries.push_back(std::move(entry));
            ++unchanged;
            continue;
        }

        // A rewrite: a real migration when payloads/stamps moved, else pure canonicalization
        // (whitespace / key order / number notation normalizing on the bulk pass — the same bytes
        // a tool save would have produced).
        const bool is_migration = result.changed;
        entry.set("action", Json(is_migration ? "migrated" : "canonicalized"));
        if (!result.diagnostics.empty())
            entry.set("diagnostics", diagnostics_json(result.diagnostics)); // orphan warnings ride along
        if (!dry_run && !write_file_atomically(file, rewritten))
        {
            entry.set("action", Json("failed"));
            entry.set("error", Json("internal.error: the rewrite could not be written"));
            entries.push_back(std::move(entry));
            ++failed;
            continue;
        }
        entries.push_back(std::move(entry));
        if (is_migration)
            ++migrated;
        else
            ++canonicalized;
    }

    Json data = Json::object();
    data.set("target", Json(target_path.generic_string()));
    data.set("dryRun", Json(dry_run));
    data.set("scanned", Json(static_cast<std::uint64_t>(candidates.size())));
    data.set("migrated", Json(migrated));
    data.set("canonicalized", Json(canonicalized));
    data.set("unchanged", Json(unchanged));
    data.set("skippedNonJson", Json(skipped));
    data.set("failed", Json(failed));
    data.set("files", std::move(entries));
    return Envelope::success(std::move(data));
}

Envelope run_migrate(const std::string& target, const std::map<std::string, std::string>& flags)
{
    // The engine-shipped migration set (EMPTY until the first real engine schema bump); package
    // sets join through pass-0 registration when the package system lands.
    return run_migrate_with(target, flags, migrate::MigrationSet::engine_set());
}

} // namespace context::cli
