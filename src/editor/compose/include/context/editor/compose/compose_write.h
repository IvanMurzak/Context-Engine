// The composed WRITE path (L-35 / R-CLI-006, the write side of the M2 composition centerpiece):
// `context set` on a composed entity writes an override in the OUTERMOST instancing scene by
// default, retargetable to the defining template (`--edit-template`) or a mid-level instancing
// scene (`--at-instance <idPath>`), reporting exactly the file + JSON-pointer it wrote. This module
// computes WHERE a write lands and returns the mutated document tree; it never touches disk (the CLI
// `context set` command does the atomic write through filesync's R-FILE-004 path). It is the write
// counterpart of flatten.h and reuses json_pointer.h (written for exactly this reuse) + the scene
// model. Also here: the advisory override-hygiene reads (`context query --overrides diverged |
// redundant`) — never auto-pruned, R-CLI-006.

#pragma once

#include "context/editor/compose/flatten.h" // SceneResolver
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/json_tree.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::compose
{

// A resolver that additionally exposes the RAW parsed document tree of a scene file (not just its
// composition view). The write path needs the raw tree to splice a mutation and re-serialize the
// exact authored document. WriteResolver IS-A SceneResolver, so it also drives flatten()/hygiene.
class WriteResolver : public SceneResolver
{
public:
    // The raw parsed JSON tree of the scene document at `path` (the same bytes build_scene_doc was
    // built from). nullptr when the path is unknown. The returned pointer must outlive the plan use.
    [[nodiscard]] virtual const serializer::JsonValue* tree(std::string_view path) const = 0;
};

// Which authored file a composed `set` writes into (R-CLI-006 / L-35).
enum class WriteTarget
{
    outermost,         // default: an override entry in the root (outermost) instancing scene
    defining_template, // --edit-template: the entity's authored value in its defining scene
    at_instance,       // --at-instance <idPath>: an override in a mid-level instancing scene
};

// A composed-write request, addressing a composed entity from the root scene's perspective.
struct WriteRequest
{
    std::string root_scene;           // the addressing (root) scene path — a resolver key
    std::vector<std::string> id_path; // L-35 id-path to the composed entity, from the root inward
    std::string pointer;              // RFC 6901 JSON pointer of the field inside the entity
    serializer::JsonValue value;      // the value to set
    WriteTarget target = WriteTarget::outermost;
    // target == at_instance: the id-path PREFIX naming the mid-level addressing scene (a strict,
    // non-empty prefix of id_path; the override addresses the entity by the remaining suffix).
    std::vector<std::string> at_instance;
};

// The outcome of planning a composed write. On success it carries the file to rewrite, the pointer
// actually written (for the R-CLI-006 provenance envelope), and the fully-mutated document tree
// ready to canonically serialize + atomically write. On failure it carries a catalog code + message
// (+ an optional JSON pointer) the CLI maps straight onto the R-CLI-008 envelope.
struct WritePlan
{
    bool ok = false;

    // --- success -------------------------------------------------------------------------------
    std::string file;              // resolver key / project-relative path of the file to rewrite
    std::string pointer;           // the JSON pointer written inside `file` (envelope: what landed)
    serializer::JsonValue document; // the mutated document tree for `file` (serialize + write this)
    WriteTarget target = WriteTarget::outermost; // where it landed (echoed in the envelope)
    bool base_recorded = false;    // an override `base` snapshot was recorded (divergence tooling)

    // --- failure -------------------------------------------------------------------------------
    std::string error_code;              // an error_catalog.h code (empty on success)
    std::string error_message;           // human/AI-readable detail
    std::optional<std::string> error_pointer; // the offending JSON pointer, when meaningful
};

// Plan a composed write. Reads the root scene + walks the id-path (via `resolver`) to verify the
// target entity exists, find its defining template, and snapshot the current template value; then
// produces the mutated tree for the target file per `request.target`. Never touches disk.
// Deterministic and total (never throws). The immutable identity pointers `/id`, `/$schema`,
// `/version` are refused (L-37 composed identity survives re-derivation).
[[nodiscard]] WritePlan plan_write(const WriteRequest& request, const WriteResolver& resolver);

// --- advisory override hygiene (R-CLI-006; never auto-pruned) -------------------------------------

enum class HygieneKind
{
    diverged,  // the override's recorded `base` no longer matches the current template value
    redundant, // the override value equals the current template value (the override is a no-op)
};

// One flagged override entry (advisory).
struct OverrideFinding
{
    std::string file;              // the scene file carrying the override entry
    std::string entry_pointer;     // "/overrides/<i>" of the entry inside `file`
    std::vector<std::string> path; // the entry's id-path (L-35)
    std::string field_pointer;     // the entry's field pointer
    std::string reason;            // human/AI-readable explanation (values, or the base/template pair)
};

// List the field-override entries reachable from the flatten of `root_scene` that are `diverged`
// (recorded base present AND != the current defining-template value) or `redundant` (override value
// == the current defining-template value). Advisory: this never fails — it returns the findings for
// the requested `kind` (possibly empty). An override that introduces a NEW field (no template value
// at its pointer) is neither diverged (no base to compare) nor redundant (it adds, not restates).
[[nodiscard]] std::vector<OverrideFinding> override_hygiene(std::string_view root_scene,
                                                            const WriteResolver& resolver,
                                                            HygieneKind kind,
                                                            const ComposeLimits& limits = {});

// True iff `pointer` addresses one of the identity fields immutable under composition (L-37):
// `/id`, `/$schema`, `/version`. Exposed for the CLI's early argument validation + tests.
[[nodiscard]] bool is_immutable_pointer(std::string_view pointer) noexcept;

} // namespace context::editor::compose
