// TS-resolved stack traces (R-OBS-005 / R-CLI-008 / R-LANG-002). The STL-only, V8-free layer that
// turns a raw V8 error `.stack` string into a TypeScript-symbolicated trace: parse the JS frames,
// remap each generated (line, column) through a SourceMap to the authored .ts position, and render
// the result for an error/diagnostic surface (the R-CLI-008 envelope + headless CLI output).
//
// This is the "headless CI still needs symbolicated traces" half of R-OBS-005 (the requirement's
// own rationale). It is backend-free and locally testable: the runtime/js V8 host captures the raw
// JS stack (CI-only for its V8 link), but parsing + remapping that stack is done HERE, so a canned
// V8 stack + a real esbuild-emitted map exercise the whole path under the local Strawberry-GCC gate.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "context/runtime/ts/source_map.h"

namespace context::runtime::ts
{

// One parsed stack frame. `line` / `column` are 1-based as they appear in the V8 stack string (and
// as a symbolicated frame is rendered). After remap_stack(), a resolved frame's `file`/`line`/
// `column`/`function-via-name` are rewritten to the TS source and `resolved` is true.
struct StackFrame
{
    std::string function;         // the function label ("" for a top-level/anonymous frame)
    std::string file;             // the script/source name as it appeared, or the mapped TS source
    std::uint32_t line = 0;       // 1-based
    std::uint32_t column = 0;     // 1-based
    bool resolved = false;        // true once remapped to a TS source position
};

// Parse a V8 error `.stack` string into frames. Recognised frame line shapes (leading whitespace +
// "at "), matching V8/Node/Chrome:
//   at <function> (<file>:<line>:<column>)
//   at <file>:<line>:<column>
// Async-prefixed frames ("at async <function> (...)") are handled. An eval-wrapped frame does not
// error (its outer parenthesised location is parsed) but is not resolved to a meaningful TS
// position — full eval-frame support belongs to the split-out interactive-debugger half.
// The leading header line (e.g. "Error: boom") and any unrecognised line are skipped — only true
// frames are returned. A frame whose location is not "<file>:<line>:<column>" is skipped.
[[nodiscard]] std::vector<StackFrame> parse_v8_stack(std::string_view stack);

// Remap every frame IN PLACE through `map`: each frame's 1-based (line, column) is converted to the
// 0-based generated position, resolved against the map, and — on a hit — rewritten to the TS source
// (1-based line/column) with `resolved = true`; the mapped name, when present, replaces an empty
// function label. Frames that do not resolve are left unchanged (pass-through). Single-bundle
// assumption: `map` is the map for the one evaluated module, so a frame is mapped by its (line,
// column) regardless of the frame's `file` label (a bare V8 eval reports "<anonymous>").
void remap_stack(std::vector<StackFrame>& frames, const SourceMap& map);

// Render frames as a symbolicated multi-line trace, one "    at ..." per frame, optionally preceded
// by a `message` header line. The inverse shape of parse_v8_stack, suitable for a CLI diagnostic.
[[nodiscard]] std::string render_stack(const std::vector<StackFrame>& frames,
                                       std::string_view message = {});

// Convenience: parse `v8Stack`, remap through `map`, and render — the one call an error surface
// makes to turn a raw JS stack into a TS-resolved trace string. `message` (e.g. "Error: boom")
// becomes the header line. When no frame resolves, the trace still renders (the raw frames), so a
// caller always gets a usable trace.
[[nodiscard]] std::string resolve_ts_stack(std::string_view v8Stack, const SourceMap& map,
                                           std::string_view message = {});

} // namespace context::runtime::ts
