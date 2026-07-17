// runtime-content-test_web_pack_stream (M8 a11): the LOCALLY-GATED proof of the web chunked-pack
// streamer. The browser export (src/render/web, Emscripten — CI-only) fetches the shipped pack over
// HTTP RANGE requests; this native ctest drives the SAME stream_pack_chunked() logic with a slice-a-
// buffer ChunkFetcher, proving (a) the archive reassembles byte-identically from arbitrary chunk sizes,
// (b) a large world streams in while the resident working set never exceeds the configured memory
// budget (R-ASSET-003), and (c) the failure paths fail closed (transport error, truncation, corrupt
// pack, budget smaller than a single unit, empty pack). Same pattern as render-web-parity: the CI-only
// emscripten path and the local gate exercise one shared implementation.

#include "context/runtime/content/web_pack_stream.h"

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"

#include "content_fixture.h"
#include "content_test.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"

#include <cstdint>
#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace content = context::runtime::content;

namespace
{

constexpr int kInstanceCount = 8;

// A root scene instancing the child scene N times — root unit + N instance units (an over-budget world).
std::string make_root_json()
{
    std::string json = R"({"$schema": "ctx:scene", "version": 1,
      "entities": [{"id": "4444444444444444", "name": "Root", "components": {}}],
      "instances": [)";
    for (int i = 0; i < kInstanceCount; ++i)
    {
        std::string id = "aaaaaaaaaaaaaaa";
        id += static_cast<char>('1' + i);
        json += "{\"id\": \"" + id + "\", \"scene\": \"child.scene.json\"}";
        if (i + 1 < kInstanceCount)
            json += ", ";
    }
    json += "]}";
    return json;
}

// A slice-a-buffer ChunkFetcher over an in-memory pack — the native analog of the browser HTTP-range
// fetcher. `corrupt_at` (>=0) flips one byte to exercise the self-verifying parse; `short_by` returns
// fewer bytes than requested to exercise the truncation guard.
class SliceFetcher final : public content::ChunkFetcher
{
public:
    explicit SliceFetcher(std::string bytes, long corrupt_at = -1, std::uint64_t short_by = 0)
        : bytes_(std::move(bytes)), corrupt_at_(corrupt_at), short_by_(short_by)
    {
        if (corrupt_at_ >= 0 && corrupt_at_ < static_cast<long>(bytes_.size()))
            bytes_[static_cast<std::size_t>(corrupt_at_)] ^= 0xFF;
    }
    [[nodiscard]] std::uint64_t total_size() const override { return bytes_.size(); }
    [[nodiscard]] bool fetch_range(std::uint64_t offset, std::uint64_t length,
                                   std::string& out) const override
    {
        if (offset + length > bytes_.size())
            return false; // out-of-range request
        std::uint64_t give = length;
        if (short_by_ != 0 && give > short_by_)
            give -= short_by_; // return a short read (the truncation guard must catch it)
        out.append(bytes_, static_cast<std::size_t>(offset), static_cast<std::size_t>(give));
        return true;
    }

private:
    std::string bytes_;
    long corrupt_at_;
    std::uint64_t short_by_;
};

} // namespace

int main()
{
    // Build a real over-budget world → a real v1 pack (the a01 writer, a test-only editor-side link).
    content_fixture::MapResolver r;
    r.add("child.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Child1", "components": {"pos": {"x": 1, "y": 2, "z": 3}}},
        {"id": "ccccccccccccccc2", "name": "Child2", "components": {"vel": {"x": 4, "y": 5, "z": 6}}}
      ]})");
    CHECK(r.add("root.scene.json", make_root_json().c_str()));
    const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    const std::string pack = content_fixture::make_pack(units, scene, /*sidecars=*/{},
                                                        /*engine_version=*/7);
    CHECK(!pack.empty());
    const std::size_t unit_count = units.units.size();
    CHECK(unit_count == static_cast<std::size_t>(kInstanceCount + 1));

    // The largest single-unit cost + the total, read from a directly-parsed source (the budget oracle).
    std::uint64_t max_unit_bytes = 0;
    std::uint64_t total_unit_bytes = 0;
    {
        content::PackContentSource src(pack);
        CHECK(src.ok());
        for (const content::UnitDescriptor& d : src.directory())
        {
            total_unit_bytes += d.resident_bytes;
            if (d.resident_bytes > max_unit_bytes)
                max_unit_bytes = d.resident_bytes;
        }
    }
    CHECK(max_unit_bytes > 0);

    // --- chunked stream within a budget: assembles from many small chunks, holds the ceiling ----------
    {
        const std::uint64_t budget = max_unit_bytes * 3;
        CHECK(total_unit_bytes > budget); // the world genuinely does not fit — eviction must occur
        const std::uint64_t chunk = 48;   // small enough to force many range fetches
        SliceFetcher fetcher(pack);
        const content::WebStreamResult res = content::stream_pack_chunked(fetcher, chunk, budget);
        CHECK(res.ok);
        CHECK(res.error.empty());
        CHECK(res.bytes_fetched == pack.size());          // the whole archive transferred
        CHECK(res.chunk_count == (pack.size() + chunk - 1) / chunk); // ⌈size/chunk⌉ range fetches
        CHECK(res.engine_version == 7);
        CHECK(res.unit_count == unit_count);
        CHECK(res.peak_resident_bytes <= budget);         // the hard memory ceiling held throughout
        CHECK(res.peak_resident_bytes >= max_unit_bytes);  // at least one unit was ever resident
        CHECK(res.resident_unit_count >= 1);
        CHECK(res.resident_unit_count < unit_count);        // NOT everything — it is over budget
        CHECK(res.world_hash != 0);
    }

    // --- whole-archive fetch (chunk_size 0) + unlimited budget: every unit resident, hash-parity -------
    {
        SliceFetcher fetcher(pack);
        const content::WebStreamResult res =
            content::stream_pack_chunked(fetcher, /*chunk_size=*/0, /*memory_budget=*/0);
        CHECK(res.ok);
        CHECK(res.chunk_count == 1);                 // one fetch of the whole archive
        CHECK(res.bytes_fetched == pack.size());
        CHECK(res.resident_unit_count == unit_count); // unlimited budget → all units resident
        CHECK(res.peak_resident_bytes == total_unit_bytes);

        // Hash parity: the chunk-assembled archive must decode IDENTICALLY to a directly-parsed pack —
        // proving the range reassembly is byte-exact (a truncation/mis-order would diverge the hash).
        content::PackContentSource direct(pack);
        CHECK(direct.ok());
        content::RuntimeContentLoader loader(direct);
        for (const content::UnitDescriptor& d : direct.directory())
            CHECK(loader.load_now(d.unit_id));
        CHECK(res.world_hash == loader.world_hash());
    }

    // --- a large chunk_size larger than the pack still streams (one clamped fetch) ---------------------
    {
        SliceFetcher fetcher(pack);
        const content::WebStreamResult res =
            content::stream_pack_chunked(fetcher, /*chunk_size=*/pack.size() + 4096, 0);
        CHECK(res.ok);
        CHECK(res.chunk_count == 1);
        CHECK(res.bytes_fetched == pack.size());
    }

    // --- failure: a corrupt pack byte fails closed at parse (self-verifying) ---------------------------
    {
        SliceFetcher fetcher(pack, /*corrupt_at=*/static_cast<long>(pack.size() / 2));
        const content::WebStreamResult res = content::stream_pack_chunked(fetcher, 48, 0);
        CHECK(!res.ok);
        CHECK(!res.error.empty());
    }

    // --- failure: a truncated range read (transport short) fails closed, no partial assembly ----------
    {
        SliceFetcher fetcher(pack, -1, /*short_by=*/4);
        const content::WebStreamResult res = content::stream_pack_chunked(fetcher, 64, 0);
        CHECK(!res.ok);
        CHECK(!res.error.empty());
    }

    // --- failure: a budget smaller than a single unit cannot stream (fail closed, never overshoot) ----
    {
        SliceFetcher fetcher(pack);
        const content::WebStreamResult res = content::stream_pack_chunked(fetcher, 48, /*budget=*/1);
        CHECK(!res.ok);
        CHECK(!res.error.empty());
    }

    // --- failure: an empty pack (total_size 0) is refused ---------------------------------------------
    {
        SliceFetcher fetcher(std::string{});
        const content::WebStreamResult res = content::stream_pack_chunked(fetcher, 48, 0);
        CHECK(!res.ok);
        CHECK(!res.error.empty());
    }

    CONTENT_TEST_MAIN_END();
}
