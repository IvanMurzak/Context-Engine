// Machine-readable diagnostic (R-FILE-003 / R-CLI-008 shape) — the recovery pass emits one instead of
// forcing state whenever it cannot safely resume an in-flight operation.

#pragma once

#include <string>
#include <vector>

namespace context::editor::filesync
{

// A structured, machine-readable diagnostic. The crash-recovery pass (R-FILE-004) produces one of
// these when an intent-log entry fails integrity, path-jail, or CAS: it NAMES the incomplete op and
// its remaining writes rather than silently applying or discarding a forged/moved-on write.
struct Diagnostic
{
    std::string code;                          // stable error-catalog code (R-CLI-008)
    std::string op_id;                         // the incomplete operation this concerns
    std::string message;                       // human-readable detail
    std::vector<std::string> remaining_writes; // paths whose durable write did not complete
};

} // namespace context::editor::filesync
