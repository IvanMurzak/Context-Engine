// Real-git-merge integration (R-QA-013 / R-FILE-012): drive an ACTUAL `git merge` through the
// `context merge-file` driver end to end and assert the result is a valid, structurally-merged JSON
// scene with NO text conflict markers. Proves the L-26 worktree-per-agent convergence path works
// against real git — not just the in-process engine. SKIPs cleanly (exit 0) when git is unavailable;
// CI and the dev host both have git, so it runs for real there.

#include "merge_test.h"

#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using context::editor::serializer::JsonValue;

namespace
{
namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* kDevNull = "nul";
#else
constexpr const char* kDevNull = "/dev/null";
#endif

std::string norm(std::string p)
{
    for (char& c : p)
        if (c == '\\')
            c = '/';
    return p;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Run a shell command; return its process exit code.
int run(const std::string& cmd)
{
#ifdef _WIN32
    // cmd.exe /c strips ONE outer quote pair when the command has multiple quoted segments, which
    // mangles `"exe" ... "path"`; wrapping the whole command in an extra pair makes cmd strip the
    // OUTER pair and preserve the inner quotes (the classic Windows system() quirk).
    return std::system(("\"" + cmd + "\"").c_str());
#else
    return std::system(cmd.c_str());
#endif
}

// git -C "<repo>" <args>
int git(const std::string& repo, const std::string& args)
{
    return run("git -C \"" + repo + "\" " + args);
}

const JsonValue* member(const JsonValue& v, const char* key)
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (const context::editor::serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

} // namespace

int main()
{
    // Skip cleanly when git is unavailable (never a false failure on a git-less host).
    if (run("git --version") != 0)
    {
        std::fprintf(stderr, "SKIP merge-test_git_driver: git not on PATH\n");
        return 0;
    }

    const std::string context_bin = norm(CONTEXT_BINARY);
    std::error_code ec;
    // A UNIQUE repo dir per run so stale state from an earlier run can never interfere.
    fs::path repo = fs::temp_directory_path() /
                    ("ctx-merge-git-driver-" +
                     std::to_string(static_cast<long long>(
                         fs::file_time_type::clock::now().time_since_epoch().count() & 0xffffff)));
    fs::remove_all(repo, ec);
    fs::create_directories(repo, ec);
    const std::string repo_s = norm(repo.string());

    // 1. init a real repo + identity. Disable any inherited hooks (a host may install a global
    //    core.hooksPath commit-identity guard) by pointing this throwaway repo at a nonexistent hooks
    //    dir, and pin autocrlf off so the canonical LF bytes are not munged on checkout.
    CHECK(git(repo_s, "init -q") == 0);
    git(repo_s, "config core.hooksPath .git/hooks-none");
    git(repo_s, "config core.autocrlf false");
    git(repo_s, "config user.email test@context.local");
    git(repo_s, "config user.name \"Context Test\"");
    git(repo_s, "config commit.gpgsign false");

    // 2. `context new` auto-installs the driver (the .gitattributes mapping + git config stanza).
    CHECK(run("\"" + context_bin + "\" new \"" + repo_s + "\" > " + kDevNull + " 2>&1") == 0);

    std::string gitattributes;
    CHECK(read_file(repo / ".gitattributes", gitattributes));
    CHECK(gitattributes.find("merge=context") != std::string::npos);

    std::string gitconfig;
    CHECK(read_file(repo / ".git" / "config", gitconfig));
    CHECK(gitconfig.find("merge \"context\"") != std::string::npos);

    // 3. Point the driver at the ABSOLUTE test binary (production installs a PATH-relative `context`;
    //    the sandbox has none on PATH). Appended as a plain file write — git uses the last stanza.
    {
        std::ofstream cfg(repo / ".git" / "config", std::ios::binary | std::ios::app);
        cfg << "\n[merge \"context\"]\n"
            << "\tname = Context structural JSON merge (test)\n"
            << "\tdriver = '" << context_bin << "' merge-file --driver %O %A %B %P\n";
    }

    // 4. A controlled id-keyed base scene — a merge git's DEFAULT line driver could not converge
    //    (a reorder on one side + a field edit on the other), so a clean result PROVES our driver ran.
    write_file(repo / "data.json",
               "{\n  \"entities\": [\n    {\"id\": \"e1\", \"hp\": 1},\n"
               "    {\"id\": \"e2\", \"hp\": 2}\n  ]\n}\n");
    CHECK(git(repo_s, "add -A") == 0);
    CHECK(git(repo_s, "commit -q --no-verify -m base") == 0);

    // 5. ours: REORDER the entities (no content change).
    CHECK(git(repo_s, "checkout -q -B ours") == 0);
    write_file(repo / "data.json",
               "{\n  \"entities\": [\n    {\"id\": \"e2\", \"hp\": 2},\n"
               "    {\"id\": \"e1\", \"hp\": 1}\n  ]\n}\n");
    CHECK(git(repo_s, "commit -q --no-verify -am ours-reorder") == 0);

    // 6. theirs: branch from base, EDIT e2.hp.
    CHECK(git(repo_s, "checkout -q -B theirs HEAD~1") == 0);
    write_file(repo / "data.json",
               "{\n  \"entities\": [\n    {\"id\": \"e1\", \"hp\": 1},\n"
               "    {\"id\": \"e2\", \"hp\": 9}\n  ]\n}\n");
    CHECK(git(repo_s, "commit -q --no-verify -am theirs-edit") == 0);

    // 7. merge theirs into ours through the driver.
    CHECK(git(repo_s, "checkout -q ours") == 0);
    const int merge_rc = git(repo_s, "merge -q --no-edit theirs");
    CHECK(merge_rc == 0); // clean structural convergence (reorder ∥ field edit)

    // 8. the merged scene is valid JSON, marker-free, and structurally correct: ours order [e2, e1]
    //    with theirs's e2.hp=9 field-merged in.
    std::string merged_text;
    CHECK(read_file(repo / "data.json", merged_text));
    CHECK(merged_text.find("<<<<<<<") == std::string::npos);
    CHECK(merged_text.find(">>>>>>>") == std::string::npos);

    context::editor::serializer::ParseResult parsed =
        context::editor::serializer::parse_json(merged_text);
    CHECK(parsed.ok); // never a text marker that would break JSON

    const JsonValue* entities = member(parsed.root, "entities");
    CHECK(entities != nullptr && entities->type == JsonValue::Type::array);
    if (entities != nullptr && entities->elements.size() == 2)
    {
        const JsonValue* id0 = member(entities->elements[0], "id");
        const JsonValue* id1 = member(entities->elements[1], "id");
        const JsonValue* hp0 = member(entities->elements[0], "hp");
        CHECK(id0 != nullptr && id0->string_value == "e2"); // ours order preserved
        CHECK(id1 != nullptr && id1->string_value == "e1");
        CHECK(hp0 != nullptr && hp0->int_value == 9); // theirs's field edit merged in
    }
    else
    {
        CHECK(false); // wrong entity count => not a structural merge
    }

    fs::remove_all(repo, ec);
    MERGE_TEST_MAIN_END();
}
