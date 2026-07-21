// A disk-backed inspector::OverrideWriteGateway over a project on disk (R-HUX-006 / R-CLI-006 / L-35 /
// L-30): the REAL implementation of the `context set` write path the GUI override-editing surfaces
// (inspector, viewport) commit through — compose::plan_write (via the SHARED
// compose::ProjectSceneResolver) + canonical serialize + `--if-match` CAS + filesync's R-FILE-004
// atomic write, and the L-30 re-read over a fresh flatten. It is byte-for-byte identical to
// `context set` because it drives the SAME plan_write over the SAME on-disk read — never a parallel
// write path. Headless (no CEF), so the GUI<->CLI override-write parity gate can exercise it directly.
//
// `expected_raw_hash == 0` means "no CAS guard" (exactly `context set` without --if-match); a non-zero
// value guards the TARGET file's current raw bytes. Each call constructs a FRESH resolver so it always
// reads the current on-disk state (the resolver snapshots per instance).

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::editor::gui::viewport
{

namespace inspector = context::editor::gui::panels::inspector;

class ProjectOverrideWriteGateway final : public inspector::OverrideWriteGateway
{
public:
    explicit ProjectOverrideWriteGateway(std::filesystem::path project_root);

    // Convert the boundary-clean request (builders::to_write_request — M9 e05d3), plan the composed
    // write (compose::plan_write over a fresh ProjectSceneResolver), canonically serialize it,
    // CAS-guard the target file's current raw bytes on `expected_raw_hash` (skipped when 0), then
    // atomically write. On success: applied + the new file raw hash + the file/pointer that landed.
    // On a CAS mismatch: cas_mismatch + the target file's CURRENT raw hash. On a write-path
    // refusal: the compose.* / file.* catalog code.
    [[nodiscard]] inspector::WriteAttempt attempt(const inspector::OverrideWriteRequest& request,
                                                  std::uint64_t expected_raw_hash) const override;

    // The current composed value at (root_scene, id_path, pointer) + the ROOT scene file's current raw
    // hash (the outermost target's CAS token — the L-30 re-read after a mismatch).
    [[nodiscard]] inspector::FieldState read(const std::string& root_scene,
                                             const std::vector<std::string>& id_path,
                                             const std::string& pointer) const override;

private:
    std::filesystem::path project_root_;
};

} // namespace context::editor::gui::viewport
