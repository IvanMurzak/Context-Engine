// One content-kind semantic finding (the R-FILE-003 diagnostic shape).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::kinds
{

// A content-kind semantic finding: a stable dotted `code` (from the R-CLI-008 error catalog), the
// RFC 6901 JSON `pointer` of the offending value in the authored document, and a human/AI `message`.
// Distinct from schema::ValidationDiagnostic (which carries SOURCE line/column): content-kind checks
// (split-nudge, fallback-cycle, plural completeness, …) run over the ALREADY-PARSED tree — the CONTENT
// rules the small schema dialect cannot express — so they locate by JSON pointer alone. Schema shape
// is validated first, upstream, by schema::validate_document.
struct KindDiagnostic
{
    std::string code;
    std::string pointer;
    std::string message;
};

// True iff `diagnostics` carries a finding with `code` (a small convenience for callers and tests).
[[nodiscard]] inline bool has_code(const std::vector<KindDiagnostic>& diagnostics,
                                   std::string_view code) noexcept
{
    for (const KindDiagnostic& d : diagnostics)
        if (d.code == code)
            return true;
    return false;
}

} // namespace context::editor::kinds
