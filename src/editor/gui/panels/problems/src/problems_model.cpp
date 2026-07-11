// Problems-panel view model: the CEF-free projection of R-FILE-003 diagnostics into a grouped,
// severity-ordered, stability-aware list + inline markers.

#include "context/editor/gui/panels/problems/problems_model.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace context::editor::gui::panels::problems
{

const char* severity_name(Severity s)
{
    switch (s)
    {
    case Severity::error:
        return "error";
    case Severity::warning:
        return "warning";
    case Severity::info:
        return "info";
    case Severity::hint:
        return "hint";
    }
    return "error";
}

bool is_provisional(bridge::Stability s) noexcept
{
    return s != bridge::Stability::stable;
}

std::string NavTarget::display() const
{
    if (file.empty())
    {
        return std::string();
    }
    std::ostringstream out;
    out << file;
    if (line > 0)
    {
        out << ':' << line;
        if (column > 0)
        {
            out << ':' << column;
        }
    }
    else if (!pointer.empty())
    {
        out << '#' << pointer;
    }
    return out.str();
}

std::string ProblemDiagnostic::identity() const
{
    if (!key.empty())
    {
        return key;
    }
    // Deterministic composite identity when no explicit key was supplied. The separator is a control
    // char unlikely to appear in a code / path / message, so distinct diagnostics never alias.
    std::ostringstream out;
    out << code << '\x1f' << nav.file << '\x1f' << nav.pointer << '\x1f' << nav.line << '\x1f'
        << nav.column << '\x1f' << message;
    return out.str();
}

ProblemsModel build_problems_model(const std::vector<ProblemDiagnostic>& diagnostics)
{
    // 1. Collapse duplicate identities (last-wins = the promotion path), preserving first-seen order.
    std::vector<ProblemDiagnostic> unique;
    unique.reserve(diagnostics.size());
    std::unordered_map<std::string, std::size_t> index_of; // identity -> slot in `unique`
    for (const ProblemDiagnostic& d : diagnostics)
    {
        const std::string id = d.identity();
        const auto it = index_of.find(id);
        if (it == index_of.end())
        {
            index_of.emplace(id, unique.size());
            unique.push_back(d);
        }
        else
        {
            unique[it->second] = d; // same problem re-emitted: newest data (e.g. promoted stability)
        }
    }

    // 2. Group by file in first-seen file order.
    ProblemsModel model;
    std::unordered_map<std::string, std::size_t> group_of; // file -> slot in model.groups
    for (const ProblemDiagnostic& d : unique)
    {
        const auto it = group_of.find(d.nav.file);
        std::size_t slot = 0;
        if (it == group_of.end())
        {
            slot = model.groups.size();
            group_of.emplace(d.nav.file, slot);
            ProblemGroup group;
            group.file = d.nav.file;
            model.groups.push_back(std::move(group));
        }
        else
        {
            slot = it->second;
        }
        model.groups[slot].diagnostics.push_back(d);

        ++model.total;
        switch (d.severity)
        {
        case Severity::error:
            ++model.errors;
            break;
        case Severity::warning:
            ++model.warnings;
            break;
        case Severity::info:
            ++model.infos;
            break;
        case Severity::hint:
            ++model.hints;
            break;
        }
        if (d.provisional())
        {
            ++model.provisional;
        }
    }

    // 3. Stable-sort each group worst-severity-first (equal severities keep first-seen order).
    for (ProblemGroup& group : model.groups)
    {
        std::stable_sort(group.diagnostics.begin(), group.diagnostics.end(),
                         [](const ProblemDiagnostic& a, const ProblemDiagnostic& b)
                         { return static_cast<int>(a.severity) < static_cast<int>(b.severity); });
    }

    return model;
}

std::vector<InlineMarker> build_inline_markers(const std::vector<ProblemDiagnostic>& diagnostics)
{
    const ProblemsModel model = build_problems_model(diagnostics);
    std::vector<InlineMarker> markers;
    for (const ProblemGroup& group : model.groups)
    {
        for (const ProblemDiagnostic& d : group.diagnostics)
        {
            if (!d.nav.navigable())
            {
                continue;
            }
            InlineMarker marker;
            marker.file = d.nav.file;
            marker.line = d.nav.line;
            marker.column = d.nav.column;
            marker.severity = d.severity;
            marker.provisional = d.provisional();
            marker.code = d.code;
            markers.push_back(std::move(marker));
        }
    }
    return markers;
}

} // namespace context::editor::gui::panels::problems
