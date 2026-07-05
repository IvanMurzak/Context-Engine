// Shared harness for the M2 exit-gate integration tests (ROADMAP §1 M2 Exit / issue #68): the tiny
// CHECK() runner (mirrors the per-module pattern) plus the two cross-cutting helpers the M2 data-model
// criteria need — a real on-disk temp project and a file-backed SceneResolver that loads every authored
// *.scene.json under a directory into compose SceneDocs (so a CLI-authored scene can be re-derived from
// its files exactly the way a restarted daemon re-derives it: files are the truth, L-19).
//
// M2's exit criteria are file-level data-model determinism proofs (author -> save -> reload -> assert),
// so unlike the M1 gate they run in-process over the real cli::run verb grammar + the compose/migrate
// libraries rather than spawning a live daemon; criterion 1 additionally spawns the REAL `context`
// binary (CONTEXT_BINARY) to prove the CLI authors a runnable scene across a true process boundary.

#pragma once

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace m2exit
{

inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

} // namespace m2exit

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            m2exit::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

#define M2_EXIT_MAIN_END() return m2exit::g_failures == 0 ? 0 : 1

namespace m2exit
{

namespace fs = std::filesystem;

// A unique temp project directory. The stamp is cast to long long before to_string — a
// file_time_type / steady_clock rep is implementation-defined and std::to_string on it is ambiguous
// under libc++ (conventions.md § Coding conventions), so never feed the raw rep to to_string.
inline fs::path make_temp_project(const char* tag)
{
    const auto stamp = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffLL);
    fs::path dir =
        fs::temp_directory_path() / ("ctx-m2exit-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

inline std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A plain out-of-band write — exactly what a text editor / CLI scaffolder / git checkout does to the
// authored files that ARE the project's truth (L-19).
inline bool write_file_raw(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << content;
    return static_cast<bool>(f);
}

// Best-effort recursive delete: never throws, so a transient Windows file lock during cleanup cannot
// abort a passing test with an uncaught filesystem_error.
inline void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

// A SceneResolver backed by real files on disk: every *.scene.json under `root` is parsed + built into
// a compose SceneDoc, keyed by its path RELATIVE to `root` with '/'-separated segments (the verbatim
// form authored instance `scene` fields use). This is the re-derivation seam — flattening through it is
// exactly what a restarted daemon does when it re-reads the project from disk (files are the truth).
class FileSceneResolver final : public context::editor::compose::SceneResolver
{
public:
    // Load (or reload) every scene file under `root`. Returns the number of scene docs built. Reusable
    // across a "restart": construct a SECOND resolver over the same dir and the composed identities
    // must be byte-identical (the L-37 stable-across-re-derivation property).
    std::size_t load(const fs::path& root)
    {
        namespace serializer = context::editor::serializer;
        namespace compose = context::editor::compose;
        docs_.clear();
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string name = entry.path().filename().string();
            if (name.size() < 11 || name.substr(name.size() - 11) != ".scene.json")
                continue;
            const std::string bytes = read_file(entry.path());
            serializer::CanonicalizeResult parsed = serializer::canonicalize(bytes);
            if (!parsed.is_json)
                continue;
            std::optional<compose::SceneDoc> doc =
                compose::build_scene_doc(rel_key(root, entry.path()), parsed.root);
            if (doc.has_value())
                docs_.emplace(rel_key(root, entry.path()), std::move(*doc));
        }
        return docs_.size();
    }

    [[nodiscard]] const context::editor::compose::SceneDoc*
    resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    // Path relative to `root`, forward-slash separated (compose keys never use backslashes even on
    // Windows — they match the authored `scene` reference bytes verbatim).
    static std::string rel_key(const fs::path& root, const fs::path& file)
    {
        std::error_code ec;
        fs::path rel = fs::relative(file, root, ec);
        std::string key = rel.generic_string();
        return key;
    }

    std::map<std::string, context::editor::compose::SceneDoc, std::less<>> docs_;
};

// An in-memory SceneResolver built from JSON strings — the lean form the seam-checklist audit uses to
// exercise a compose seam without touching disk (mirrors the per-module compose tests' MapResolver).
class MapResolver final : public context::editor::compose::SceneResolver
{
public:
    // Add (path -> authored scene JSON). Returns false if the JSON is not a scene document.
    bool add(const std::string& path, const std::string& json)
    {
        namespace serializer = context::editor::serializer;
        namespace compose = context::editor::compose;
        serializer::CanonicalizeResult parsed = serializer::canonicalize(json);
        if (!parsed.is_json)
            return false;
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
            return false;
        docs_[path] = std::move(*doc);
        return true;
    }

    [[nodiscard]] const context::editor::compose::SceneDoc*
    resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, context::editor::compose::SceneDoc, std::less<>> docs_;
};

} // namespace m2exit
