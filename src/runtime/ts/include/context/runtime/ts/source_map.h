// Source Map v3 resolution (R-OBS-005 / L-61) — the STL-only, V8-free half of TS-source-mapped
// debugging. Parses the JSON Source Map v3 an esbuild transpile emits (src/runtime/ts) and resolves
// a GENERATED (line, column) position in the bundled JS back to its ORIGINAL authored-TS position
// (source file, line, column, and — when present — the mapped name).
//
// Deliberately backend-free: no V8 and no esbuild types cross this header, so it builds + runs on
// EVERY toolchain (the local Strawberry-GCC Windows dev gate included) and its ctest is a LOCAL
// gate. The runtime/js V8 host produces a raw JS stack trace (CI-only for its V8 link); mapping
// that stack back to TS positions is done HERE (source_map + stack_trace), so the whole mapping
// path is fully locally testable without linking V8.
//
// Scope (task 4b core, the source-map foundation): the standard Source Map v3 fields — version,
// sources, names, and the base64-VLQ `mappings` segment grid. `sourcesContent`, `sourceRoot`, and
// index maps (the `sections` form) are OUT of the DoD floor and documented as deferred seams
// (README.md § Deferred seams). This half feeds TS-resolved stack traces; the interactive CDP
// inspector attach + source-mapped breakpoints are a split-out follow-up.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::runtime::ts
{

// A resolved original position: a 0-based line/column into `source` (one entry of
// SourceMap::sources). `name` is the mapped identifier when the segment carried a names index,
// otherwise empty.
struct OriginalPosition
{
    std::string source;
    std::uint32_t line = 0;   // 0-based line into the original source
    std::uint32_t column = 0; // 0-based column into the original source
    std::string name;         // the mapped symbol name, or empty
};

// A parsed Source Map v3. Construct via parse(); query via resolve().
class SourceMap
{
public:
    // Parse a Source Map v3 JSON document. Returns std::nullopt (and fills `*err` when non-null) on
    // malformed JSON, an unsupported `version` (only 3 is accepted), a non-array sources/names, or a
    // malformed base64-VLQ `mappings` string. A map with an empty `mappings` string parses to a map
    // that resolves nothing (valid, not an error).
    [[nodiscard]] static std::optional<SourceMap> parse(std::string_view json,
                                                        std::string* err = nullptr);

    // Resolve a GENERATED (0-based `line`, 0-based `column`) to its original position, using the
    // standard "greatest generated column <= `column` on `line`" nearest-preceding rule. Returns
    // std::nullopt when `line` carries no mappings, when `column` precedes the first segment on the
    // line, or when the nearest segment is a generated-column-only segment (no source field).
    [[nodiscard]] std::optional<OriginalPosition> resolve(std::uint32_t line,
                                                          std::uint32_t column) const;

    [[nodiscard]] const std::vector<std::string>& sources() const { return sources_; }
    [[nodiscard]] const std::vector<std::string>& names() const { return names_; }

private:
    // One decoded mapping segment on a generated line. A segment is 1 field (genColumn only), 4
    // fields (+ source/origLine/origColumn), or 5 fields (+ name).
    struct Segment
    {
        std::uint32_t genColumn = 0;  // 0-based generated column
        bool hasSource = false;       // false for a one-field segment
        std::uint32_t sourceIndex = 0;
        std::uint32_t origLine = 0;   // 0-based
        std::uint32_t origColumn = 0; // 0-based
        bool hasName = false;
        std::uint32_t nameIndex = 0;
    };

    std::vector<std::string> sources_;
    std::vector<std::string> names_;
    // segments_[genLine] = the segments on that generated line, ordered by genColumn ascending.
    std::vector<std::vector<Segment>> segments_;
};

} // namespace context::runtime::ts
