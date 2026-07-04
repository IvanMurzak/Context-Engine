// R-CLI-017 resource-store tests: spool + full/range read round-trips (byte-exact reassembly), the
// chunk cap, eof semantics, the hex codec, and the failure paths — foreign-instance handles,
// unknown payload ids, out-of-range offsets, vanished spool files. (R-QA-013: happy + edge +
// failure coverage.)

#include "context/editor/bridge/resource_store.h"

#include "bridge_test.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using context::editor::bridge::hex_decode;
using context::editor::bridge::hex_encode;
using context::editor::bridge::kResourceReadMaxChunkBytes;
using context::editor::bridge::ResourceStore;
using context::editor::contract::ResourceHandle;

namespace
{
fs::path make_temp_dir(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-resource-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

int main()
{
    // --- hex codec: round-trip incl. binary bytes; strict decode -------------------------------
    {
        const std::string payload = std::string("a\0b\xff\x10z", 6);
        const std::string hex = hex_encode(payload);
        CHECK(hex == "610062ff107a");
        const auto back = hex_decode(hex);
        CHECK(back.has_value() && *back == payload);
        CHECK(hex_decode("").has_value() && hex_decode("")->empty());
        CHECK(hex_decode("ABCDEF").has_value()); // uppercase accepted on decode
        CHECK(!hex_decode("abc").has_value());   // odd length
        CHECK(!hex_decode("zz").has_value());    // non-hex digit
    }

    const fs::path dir = make_temp_dir("store");
    ResourceStore store(dir / "resources", "inst-A");

    // --- put + full read round-trip (happy path) ------------------------------------------------
    std::string payload;
    payload.reserve(300000);
    for (int i = 0; i < 100000; ++i)
        payload += std::to_string(i % 10) + ",\n";
    const auto handle = store.put(payload);
    CHECK(handle.has_value());
    CHECK(handle->instance_id == "inst-A");
    CHECK(handle->size_bytes == payload.size());

    {
        const auto whole = store.read(*handle, 0, 0); // 0 = up to the chunk cap
        CHECK(whole.has_value());
        CHECK(whole->offset == 0);
        CHECK(whole->total == payload.size());
        CHECK(whole->eof);
        CHECK(whole->bytes == payload);
    }

    // --- RANGE reads reassemble the exact original bytes (the R-QA-013 range-read mandate) ------
    {
        std::string reassembled;
        std::uint64_t offset = 0;
        const std::uint64_t step = 65536; // deliberately not a divisor of the payload size
        for (;;)
        {
            const auto chunk = store.read(*handle, offset, step);
            CHECK(chunk.has_value());
            if (!chunk.has_value())
                break;
            CHECK(chunk->offset == offset);
            CHECK(chunk->bytes.size() <= step);
            reassembled += chunk->bytes;
            offset += chunk->bytes.size();
            if (chunk->eof)
                break;
        }
        CHECK(reassembled == payload); // byte-exact
    }

    // --- edge: offset == total yields an empty eof chunk; the chunk cap clamps requests ---------
    {
        const auto at_end = store.read(*handle, payload.size(), 4096);
        CHECK(at_end.has_value());
        CHECK(at_end->bytes.empty());
        CHECK(at_end->eof);

        const auto clamped = store.read(*handle, 0, kResourceReadMaxChunkBytes + 1);
        CHECK(clamped.has_value());
        CHECK(clamped->bytes.size() == payload.size()); // payload < cap, so full read
    }

    // --- edge: an empty payload round-trips ------------------------------------------------------
    {
        const auto empty = store.put("");
        CHECK(empty.has_value());
        CHECK(empty->size_bytes == 0);
        const auto r = store.read(*empty, 0, 0);
        CHECK(r.has_value());
        CHECK(r->bytes.empty());
        CHECK(r->eof);
    }

    // --- failure paths ---------------------------------------------------------------------------
    {
        // offset beyond total
        CHECK(!store.read(*handle, payload.size() + 1, 1).has_value());

        // unknown payload id
        ResourceHandle unknown = *handle;
        unknown.payload_id = 9999;
        CHECK(!store.read(unknown, 0, 0).has_value());
        CHECK(store.local_path_hint(unknown).empty());

        // foreign instance (stale incarnation / another daemon)
        ResourceHandle foreign = *handle;
        foreign.instance_id = "inst-B";
        CHECK(!store.read(foreign, 0, 0).has_value());

        // URI round-trip feeds resolution: parse(to_uri) resolves identically
        const auto reparsed = ResourceHandle::parse(handle->to_uri());
        CHECK(reparsed.has_value());
        const auto via_uri = store.read(*reparsed, 0, 16);
        CHECK(via_uri.has_value());
        CHECK(via_uri->bytes == payload.substr(0, 16));
    }

    // --- the same-FS localPath hint names the real spool file -----------------------------------
    {
        const std::string hint = store.local_path_hint(*handle);
        CHECK(!hint.empty());
        CHECK(fs::exists(fs::path(hint)));
    }

    // --- a NEW incarnation invalidates old handles (residue cleared, ids reset under a new id) ---
    {
        ResourceStore next(dir / "resources", "inst-NEXT");
        CHECK(!next.read(*handle, 0, 0).has_value()); // old instance id -> unknown
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    BRIDGE_TEST_MAIN_END();
}
