// The v1 reference isolated runner + the R-ASSET-001 determinism double-run gate.

#include "context/editor/import/isolated_runner.h"

#include <string>

namespace context::editor::import
{

IsolatedImport run_isolated(const Importer& importer, const ImportInput& input,
                            const SandboxPolicy& policy)
{
    IsolatedImport out;
    const OsSandboxSupport os = os_sandbox_support();
    out.audit.input_path = policy.input_path;
    out.audit.output_key = policy.output_key;
    out.audit.network_allowed = policy.allow_network;
    out.audit.os_primitive_enforced = os.enforced;
    out.audit.os_primitive = os.primitive;

    // Enforce the policy CONTRACT the subprocess runner will later enforce at the syscall layer.
    // v1 never grants network (R-SEC-010).
    if (policy.allow_network)
    {
        out.result.ok = false;
        out.result.diagnostics.push_back(
            {"import.jail_escape", "network capability requested but denied in v1 (R-SEC-010)"});
        return out;
    }
    // The source the run reads must be inside the jail (R-SEC-008). Skipped for a purely in-memory
    // import (empty source_path) where there is no path to jail — the bytes are already the input.
    if (!input.source_path.empty() && !read_permitted(policy, input.source_path))
    {
        out.result.ok = false;
        out.result.diagnostics.push_back(
            {"import.jail_escape", "source path escapes the importer jail root (R-SEC-008)"});
        return out;
    }

    // The importer is pure over source_bytes (importer.h) — it touches no path/env/clock, so running
    // it here is already confined to "input bytes + own output key". The subprocess runner swaps in
    // without any importer change (os_sandbox_support staging).
    out.result = importer.import(input);
    return out;
}

DeterminismReport check_deterministic(const Importer& importer, const ImportInput& input)
{
    const ImportResult first = importer.import(input);
    const ImportResult second = importer.import(input);
    DeterminismReport report;

    if (first.ok != second.ok)
    {
        report.divergence = "ok flag differs between runs";
        return report;
    }
    if (first.artifacts.size() != second.artifacts.size())
    {
        report.divergence = "artifact count differs (" + std::to_string(first.artifacts.size()) +
                            " vs " + std::to_string(second.artifacts.size()) + ")";
        return report;
    }
    for (std::size_t i = 0; i < first.artifacts.size(); ++i)
    {
        const DerivedArtifact& a = first.artifacts[i];
        const DerivedArtifact& b = second.artifacts[i];
        const std::string at = "artifact[" + std::to_string(i) + "]";
        if (a.kind != b.kind)
        {
            report.divergence = at + " kind differs";
            return report;
        }
        if (a.name != b.name)
        {
            report.divergence = at + " name differs ('" + a.name + "' vs '" + b.name + "')";
            return report;
        }
        if (a.derived_format_version != b.derived_format_version)
        {
            report.divergence = at + " ('" + a.name + "') derived-format version differs";
            return report;
        }
        if (a.bytes != b.bytes)
        {
            report.divergence = at + " ('" + a.name + "') bytes differ (" +
                                std::to_string(a.bytes.size()) + " vs " +
                                std::to_string(b.bytes.size()) + " bytes)";
            return report;
        }
    }
    // Failure paths must also be deterministic: a bad source fails identically twice.
    if (first.diagnostics.size() != second.diagnostics.size())
    {
        report.divergence = "diagnostic count differs";
        return report;
    }
    for (std::size_t i = 0; i < first.diagnostics.size(); ++i)
    {
        if (first.diagnostics[i].code != second.diagnostics[i].code ||
            first.diagnostics[i].message != second.diagnostics[i].message)
        {
            report.divergence = "diagnostic[" + std::to_string(i) + "] differs";
            return report;
        }
    }

    report.deterministic = true;
    return report;
}

} // namespace context::editor::import
