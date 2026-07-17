// WebPackStreamer (R-ASSET-003 / R-ASSET-005 — the Web conditional-MUST, M8 task a11): stream a v1
// chunked pack into the RuntimeKernel content loader over a chunked byte-fetch seam, holding the
// resident working set within a configured memory budget. This is the runtime half of the a11 web
// export: the browser fetches the shipped pack (served alongside the wasm+js bundle) in HTTP RANGE
// chunks, feeds the assembled bytes to the a02 PackContentSource, and materializes only the resident
// units — never the whole archive — so a large world runs inside the browser memory budget.
//
// The chunked transfer is abstracted behind the ChunkFetcher seam:
//   - in the browser (Emscripten export) an HTTP-range fetcher pulls the pack over `fetch()`;
//   - in the native gate a slice-a-buffer fetcher exercises the SAME assembly + budget logic.
// So the streamed-in-chunks path is LOCALLY verifiable (the ctest below) though emcc is CI-only — the
// same pattern as src/render/web (the CI-only web_main.cpp + the native render-web-parity ctest).
//
// L-24 layering: this depends only on the a02 content loader (which itself pulls only the frozen pack
// format + the canonical serializer), never the editor writer / context_compose — it links cleanly into
// a RuntimeKernel build (native OR Emscripten).

#pragma once

#include <cstdint>
#include <string>

namespace context::runtime::content
{

// The chunked byte-fetch seam the streamer pulls the pack over. Implementations fetch arbitrary byte
// ranges of the pack: an HTTP range request in the browser, or a buffer slice in the native ctest.
class ChunkFetcher
{
public:
    virtual ~ChunkFetcher() = default;

    // The pack's total byte length (a HEAD probe / Content-Length in the browser). 0 ⇒ unknown/empty.
    [[nodiscard]] virtual std::uint64_t total_size() const = 0;

    // Append the [offset, offset+length) byte range of the pack to `out` (which the streamer clears
    // before each call). Returns false on a transport error or an out-of-range request — the streamer
    // then fails closed rather than assembling a truncated pack.
    [[nodiscard]] virtual bool fetch_range(std::uint64_t offset, std::uint64_t length,
                                           std::string& out) const = 0;
};

// The outcome of a chunked stream. ok ⇒ the pack fetched + parsed and every unit streamed within the
// budget; !ok ⇒ `error` names the failure (a transport error, a truncated/corrupt pack, or a budget
// too small for even one unit). The counters are the R-ASSET-003 evidence: peak_resident_bytes is the
// high-water resident working set (≤ memory_budget when the budget is set and ≥ the largest unit), and
// chunk_count is how many range fetches assembled the archive (the streamed-in-chunks proof).
struct WebStreamResult
{
    bool ok = false;
    std::string error;
    std::uint64_t bytes_fetched = 0;      // total bytes pulled over the fetcher (== the pack size on ok)
    std::size_t chunk_count = 0;          // number of range fetches (⌈size / chunk_size⌉)
    std::uint64_t engine_version = 0;     // the parsed pack's engine version (R-FILE-010 cache-key input)
    std::size_t unit_count = 0;           // units in the pack directory
    std::size_t resident_unit_count = 0;  // units resident at the end of the stream (≤ unit_count)
    std::uint64_t peak_resident_bytes = 0; // high-water resident working set over the whole stream
    std::uint64_t world_hash = 0;         // feed-independent hash of the finally-resident world
};

// Stream a whole pack over `fetcher` in `chunk_size`-byte range fetches (chunk_size 0 ⇒ one fetch of
// the whole archive), then materialize every unit through a RuntimeContentLoader while holding the
// resident working set at or below `memory_budget` bytes (0 ⇒ unlimited): after each unit loads, the
// OLDEST-loaded resident units are evicted until resident_bytes ≤ the budget, so the working set stays
// bounded exactly as a browser memory budget requires. Total — never throws; a transport/parse/budget
// failure returns ok=false + `error`.
[[nodiscard]] WebStreamResult stream_pack_chunked(const ChunkFetcher& fetcher,
                                                  std::uint64_t chunk_size,
                                                  std::uint64_t memory_budget);

} // namespace context::runtime::content
