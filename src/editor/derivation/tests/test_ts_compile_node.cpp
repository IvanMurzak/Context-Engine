// The TypeScript-compile derivation node (R-FILE-010; issue #85). Exercises, THROUGH the node, the
// content-addressed cache contract: a cache hit skips the re-transpile (unchanged TS is not
// re-transpiled — the DoD), the key enumerates every input (source bytes + options + toolchain
// version), distinct inputs are distinct entries, a deterministic failure is cached, and explicit
// invalidation reclaims an entry. Backend-agnostic: driven by a FAKE TsToolchain that COUNTS its
// transpile() calls, so this is a pure LOCAL gate — no esbuild binary needed (unlike the ts-* ctests).

#include "context/editor/derivation/ts_compile_node.h"
#include "context/runtime/ts/ts_toolchain.h"

#include "derivation_test.h"

#include <string>

using namespace context::editor::derivation;
using context::runtime::ts::ModuleFormat;
using context::runtime::ts::TranspileOptions;
using context::runtime::ts::TranspileResult;
using context::runtime::ts::TsToolchain;

namespace
{

// A fake toolchain that counts transpile() calls and returns a UNIQUE result per call, so a cache hit
// (which returns the stored first result) is distinguishable from a recompute (a new call count).
class FakeToolchain final : public TsToolchain
{
public:
    explicit FakeToolchain(bool ok = true) : ok_(ok) {}

    std::string_view name() const override { return "fake"; }
    std::string version(std::string&) const override { return "fake-1.0"; }

    TranspileResult transpile(const std::string& tsFilePath, const TranspileOptions& /*opts*/) override
    {
        ++calls_;
        TranspileResult r;
        r.ok = ok_;
        if (ok_)
        {
            r.js = "/*compiled#" + std::to_string(calls_) + " " + tsFilePath + "*/";
        }
        else
        {
            r.diagnostics.push_back({std::string(context::runtime::ts::kTsTranspileFailedCode),
                                     "fake failure #" + std::to_string(calls_), tsFilePath, 0, 0});
        }
        return r;
    }

    int calls() const { return calls_; }

private:
    bool ok_;
    int calls_ = 0;
};

// The headline R-FILE-010 behaviour: a repeated compile of the SAME source is served from the cache
// without a second transpile() call — and the artifact is exactly the first result.
void test_cache_hit_skips_recompute()
{
    FakeToolchain tc;
    TsCompileNode node(tc, "esbuild-0.28.1");
    TranspileOptions opts;

    const TranspileResult& first = node.get_or_compile("game.ts", "export const x = 1;", opts);
    CHECK(node.misses() == 1);
    CHECK(node.hits() == 0);
    CHECK(node.size() == 1);
    CHECK(tc.calls() == 1);
    const std::string first_js = first.js;

    const TranspileResult& second = node.get_or_compile("game.ts", "export const x = 1;", opts);
    CHECK(node.hits() == 1);
    CHECK(node.misses() == 1);
    CHECK(node.size() == 1);
    CHECK(tc.calls() == 1);          // NO re-transpile — the whole point of the cache
    CHECK(second.js == first_js);    // served the identical stored artifact
}

// A change to the source bytes is a distinct key => a fresh transpile (a whitespace-different source
// is genuinely different bytes; canonicalization is the caller's concern upstream).
void test_distinct_source_is_a_miss()
{
    FakeToolchain tc;
    TsCompileNode node(tc, "esbuild-0.28.1");
    TranspileOptions opts;

    (void)node.get_or_compile("game.ts", "export const x = 1;", opts);
    (void)node.get_or_compile("game.ts", "export const x = 2;", opts); // different bytes
    CHECK(node.misses() == 2);
    CHECK(node.size() == 2);
    CHECK(tc.calls() == 2);
}

// The transpile OPTIONS are a key component: the same source bundled vs plain-transpiled, or ESM vs
// IIFE, or with vs without a sourcemap, are distinct artifacts (R-FILE-010).
void test_distinct_options_are_distinct_entries()
{
    FakeToolchain tc;
    TsCompileNode node(tc, "esbuild-0.28.1");
    const std::string src = "export const x = 1;";

    TranspileOptions plain;
    TranspileOptions bundled;
    bundled.bundle = true;
    TranspileOptions iife;
    iife.format = ModuleFormat::Iife;
    TranspileOptions mapped;
    mapped.sourcemap = true;

    (void)node.get_or_compile("game.ts", src, plain);
    (void)node.get_or_compile("game.ts", src, bundled);
    (void)node.get_or_compile("game.ts", src, iife);
    (void)node.get_or_compile("game.ts", src, mapped);
    CHECK(node.size() == 4);
    CHECK(node.misses() == 4);
    CHECK(tc.calls() == 4);
}

// The TOOLCHAIN VERSION is a key component (R-FILE-010's "importer version"): an esbuild upgrade that
// changes the emitted JS yields NEW keys, so the same source is re-transpiled under the new tool
// rather than serving stale output.
void test_toolchain_version_is_a_key_component()
{
    const std::string src = "export const x = 1;";
    TranspileOptions opts;

    CHECK(TsCompileNode::cache_key(src, opts, "esbuild-0.28.1") !=
          TsCompileNode::cache_key(src, opts, "esbuild-0.29.0"));

    // Two nodes on different toolchain versions never share an entry.
    FakeToolchain tc_old;
    FakeToolchain tc_new;
    TsCompileNode old_node(tc_old, "esbuild-0.28.1");
    TsCompileNode new_node(tc_new, "esbuild-0.29.0");
    (void)old_node.get_or_compile("game.ts", src, opts);
    (void)new_node.get_or_compile("game.ts", src, opts);
    CHECK(!new_node.contains(TsCompileNode::cache_key(src, opts, "esbuild-0.28.1")));
    CHECK(new_node.contains(TsCompileNode::cache_key(src, opts, "esbuild-0.29.0")));
}

// The content-addressed key enumerates every input, is deterministic + instance-independent, and is
// discoverable via contains()/find().
void test_cache_key_enumerates_inputs()
{
    const std::string src = "export const x = 1;";
    TranspileOptions opts;

    const std::string k0 = TsCompileNode::cache_key(src, opts, "v1");
    CHECK(k0 == TsCompileNode::cache_key(src, opts, "v1")); // deterministic + instance-independent
    CHECK(!k0.empty());

    CHECK(k0 != TsCompileNode::cache_key("export const x = 2;", opts, "v1")); // source is a component
    TranspileOptions bundled;
    bundled.bundle = true;
    CHECK(k0 != TsCompileNode::cache_key(src, bundled, "v1"));  // options are a component
    CHECK(k0 != TsCompileNode::cache_key(src, opts, "v2"));     // toolchain version is a component

    FakeToolchain tc;
    TsCompileNode node(tc, "v1");
    const TranspileResult& stored = node.get_or_compile("game.ts", src, opts);
    CHECK(node.contains(k0));
    CHECK(node.find(k0) != nullptr);
    CHECK(node.find(k0)->js == stored.js);
    CHECK(!node.contains(TsCompileNode::cache_key("export const x = 2;", opts, "v1")));
    CHECK(node.find("no-such-key") == nullptr);
}

// invalidate() reclaims exactly one (source, options) entry; the next request recompiles.
void test_invalidate_drops_entry()
{
    FakeToolchain tc;
    TsCompileNode node(tc, "esbuild-0.28.1");
    const std::string src = "export const x = 1;";
    TranspileOptions opts;

    (void)node.get_or_compile("game.ts", src, opts);
    CHECK(node.size() == 1);
    CHECK(tc.calls() == 1);

    CHECK(node.invalidate(src, opts) == 1); // one entry evicted
    CHECK(node.size() == 0);
    CHECK(node.invalidate(src, opts) == 0); // idempotent — nothing left to drop

    (void)node.get_or_compile("game.ts", src, opts); // recompiles under the same key
    CHECK(tc.calls() == 2);
    CHECK(node.size() == 1);
}

// A deterministic transpile FAILURE is cached like any other pure result: the same bad source is not
// re-run, and the cached failure is served (re-authoring changes the content -> a new key).
void test_failed_transpile_is_cached()
{
    FakeToolchain tc(/*ok=*/false);
    TsCompileNode node(tc, "esbuild-0.28.1");
    TranspileOptions opts;

    const TranspileResult& first = node.get_or_compile("bad.ts", "syntax ( error", opts);
    CHECK(!first.ok);
    CHECK(!first.diagnostics.empty());
    CHECK(tc.calls() == 1);

    const TranspileResult& second = node.get_or_compile("bad.ts", "syntax ( error", opts);
    CHECK(!second.ok);
    CHECK(tc.calls() == 1);      // the deterministic failure was served from the cache, not re-run
    CHECK(node.hits() == 1);
}

} // namespace

int main()
{
    test_cache_hit_skips_recompute();
    test_distinct_source_is_a_miss();
    test_distinct_options_are_distinct_entries();
    test_toolchain_version_is_a_key_component();
    test_cache_key_enumerates_inputs();
    test_invalidate_drops_entry();
    test_failed_transpile_is_cached();
    DERIVATION_TEST_MAIN_END();
}
