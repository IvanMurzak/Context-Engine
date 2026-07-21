// The disk-backed override-write gateway — see project_override_gateway.h. Drives the SAME
// compose::plan_write + canonical serialize + R-FILE-004 atomic write that `context set`
// (src/cli/set_command.cpp) runs, over the SHARED compose::ProjectSceneResolver, so a GUI commit and
// the CLI verb produce byte-identical authored files.

#include "context/editor/gui/viewport/project_override_gateway.h"

#include "context/editor/compose/compose_write.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/project_resolver.h"
#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/gui/panels/builders/inspector_builder.h" // builders::to_write_request
#include "context/editor/serializer/canonical.h"

#include <optional>
#include <string>
#include <utility>

namespace context::editor::gui::viewport
{

namespace compose = context::editor::compose;
namespace filesync = context::editor::filesync;
namespace serializer = context::editor::serializer;

ProjectOverrideWriteGateway::ProjectOverrideWriteGateway(std::filesystem::path project_root)
    : project_root_(std::move(project_root))
{
}

inspector::WriteAttempt ProjectOverrideWriteGateway::attempt(
    const inspector::OverrideWriteRequest& request, std::uint64_t expected_raw_hash) const
{
    inspector::WriteAttempt out;

    // Plan the composed write over a FRESH resolver (always the current on-disk state). The
    // boundary-clean request converts through the ONE builders mapping (M9 e05d3) — this gateway is
    // the kernel side of the seam, so reaching compose here is exactly where that reach belongs.
    const compose::ProjectSceneResolver resolver(project_root_);
    const compose::WritePlan plan =
        compose::plan_write(panels::builders::to_write_request(request), resolver);
    if (!plan.ok)
    {
        out.code = plan.error_code;
        out.message = plan.error_message;
        return out;
    }

    std::string new_bytes;
    if (!serializer::serialize_canonical(plan.document, new_bytes))
    {
        out.code = "internal.error";
        out.message = "the mutated scene document could not be canonically serialized";
        return out;
    }

    filesync::NativeFileStore store(project_root_);

    // --if-match CAS (R-FILE-004 / R-CLI-006 — the raw-byte hash guards the exact bytes on disk). A
    // zero `expected_raw_hash` means "no guard" (exactly `context set` without --if-match).
    if (expected_raw_hash != 0)
    {
        const std::optional<std::string> current = store.read(plan.file);
        const std::uint64_t actual = current ? filesync::content_hash(*current) : 0;
        if (!current || actual != expected_raw_hash)
        {
            out.cas_mismatch = true;
            out.code = "cas.mismatch";
            out.message = "the target file's current bytes do not match the expected hash";
            out.raw_hash = actual; // the CURRENT raw hash (the L-30 re-read token)
            return out;
        }
    }

    if (!filesync::atomic_write(store, plan.file, new_bytes))
    {
        out.code = "internal.error";
        out.message = "the atomic write to `" + plan.file + "` failed (path jail refusal or IO error)";
        return out;
    }

    out.applied = true;
    out.file = plan.file;
    out.pointer = plan.pointer;
    out.raw_hash = filesync::content_hash(new_bytes);
    return out;
}

inspector::FieldState ProjectOverrideWriteGateway::read(const std::string& root_scene,
                                                        const std::vector<std::string>& id_path,
                                                        const std::string& pointer) const
{
    inspector::FieldState out;

    const compose::ProjectSceneResolver resolver(project_root_);
    const compose::ComposedScene scene = compose::flatten(root_scene, resolver);
    for (const compose::ComposedEntity& entity : scene.entities)
    {
        if (entity.id_path != id_path)
        {
            continue;
        }
        if (const serializer::JsonValue* value = compose::resolve_json_pointer(entity.value, pointer))
        {
            out.present = true;
            out.value = *value;
        }
        break;
    }

    // The CAS token is the OUTERMOST (root) scene file's current raw hash — the file the default
    // override write lands in (matches inspector::FieldState's documented semantics).
    const filesync::NativeFileStore store(project_root_);
    if (const std::optional<std::string> bytes = store.read(root_scene))
    {
        out.raw_hash = filesync::content_hash(*bytes);
    }
    return out;
}

} // namespace context::editor::gui::viewport
