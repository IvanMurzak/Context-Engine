// The composed WRITE path + advisory override hygiene — see compose_write.h.

#include "context/editor/compose/compose_write.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::compose
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// --- small JSON-tree builders (mirroring flatten.cpp's local helpers) ----------------------------

[[nodiscard]] JsonValue json_string(std::string_view v)
{
    JsonValue out;
    out.type = JsonValue::Type::string;
    out.string_value = std::string(v);
    return out;
}

// Two JSON values are "equal" iff their canonical serializations match — the engine's ONE notion of
// value identity (R-FILE-001). Total: a value carrying a non-finite double (never present in a
// parsed/authored tree) serializes to empty and still compares deterministically.
[[nodiscard]] bool canonical_equal(const JsonValue& a, const JsonValue& b)
{
    std::string sa;
    std::string sb;
    const bool oka = serializer::serialize_canonical(a, sa);
    const bool okb = serializer::serialize_canonical(b, sb);
    return oka == okb && sa == sb;
}

// The mutable "overrides" array member of a scene tree, created (as an empty array) when absent.
[[nodiscard]] JsonValue& overrides_array(JsonValue& scene_tree)
{
    for (JsonMember& m : scene_tree.members)
        if (m.key == "overrides")
        {
            // A scene whose `overrides` is not an array is a schema violation caught upstream; treat
            // it as empty by resetting to an array so the write stays well-formed.
            if (m.value.type != JsonValue::Type::array)
            {
                m.value = JsonValue{};
                m.value.type = JsonValue::Type::array;
            }
            return m.value;
        }
    JsonMember added;
    added.key = "overrides";
    added.value.type = JsonValue::Type::array;
    scene_tree.members.push_back(std::move(added));
    return scene_tree.members.back().value;
}

// Build one field-override entry object: {"path": [...], "pointer": ..., "value": ..., ["base": ...]}.
// `base` is snapshotted only when the template currently authors the pointer (a NEW-field override
// has no base to record — and thus is never divergence-checkable, which is correct).
[[nodiscard]] JsonValue make_override_entry(const std::vector<std::string>& path,
                                            const std::string& pointer, const JsonValue& value,
                                            const JsonValue* base)
{
    JsonValue entry;
    entry.type = JsonValue::Type::object;

    JsonValue path_array;
    path_array.type = JsonValue::Type::array;
    for (const std::string& seg : path)
        path_array.elements.push_back(json_string(seg));

    JsonMember m_path{"path", std::move(path_array)};
    JsonMember m_pointer{"pointer", json_string(pointer)};
    JsonMember m_value{"value", value};
    entry.members.push_back(std::move(m_path));
    entry.members.push_back(std::move(m_pointer));
    entry.members.push_back(std::move(m_value));
    if (base != nullptr)
        entry.members.push_back(JsonMember{"base", *base});
    return entry;
}

// The raw authored-array index encoded in a "/overrides/<i>" pointer (scene_model records the RAW
// index, so it addresses the same element in the raw tree). npos-safe: returns false on a shape it
// does not recognize.
[[nodiscard]] bool override_index_of(const std::string& entry_pointer, std::size_t& out)
{
    static const std::string kPrefix = "/overrides/";
    if (entry_pointer.rfind(kPrefix, 0) != 0)
        return false;
    const std::string digits = entry_pointer.substr(kPrefix.size());
    if (digits.empty())
        return false;
    std::size_t value = 0;
    for (const char c : digits)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    out = value;
    return true;
}

// Where a composed entity is authored: the defining scene, the entity's pointer inside it, and the
// authored entity value (borrowed from the resolver's SceneDoc). Walking `id_path` from `scene`:
// every segment but the last is an instance id (descend into its scene); the last is the entity id
// (an authored entity, or the scene-root entity addressed by its id or the `$root` token).
struct TemplateLocation
{
    bool found = false;
    std::string reason;                       // when !found (a human/AI explanation)
    std::string file;                         // the defining scene (resolver key)
    std::string entity_pointer;               // "/entities/<i>" or "/root"
    const JsonValue* entity_value = nullptr;  // the authored entity value (from the SceneDoc)
};

[[nodiscard]] TemplateLocation locate_template(const std::string& scene,
                                               const std::vector<std::string>& id_path,
                                               const WriteResolver& resolver)
{
    TemplateLocation loc;
    if (id_path.empty())
    {
        loc.reason = "an empty id-path addresses no entity";
        return loc;
    }
    std::string current = scene;
    const SceneDoc* doc = resolver.resolve(current);
    if (doc == nullptr)
    {
        loc.reason = "scene `" + current + "` does not resolve to a known scene";
        return loc;
    }

    for (std::size_t i = 0; i < id_path.size(); ++i)
    {
        const std::string& seg = id_path[i];
        const bool last = (i + 1 == id_path.size());
        if (last)
        {
            if (doc->root.present)
            {
                const std::string root_id =
                    doc->root.id.empty() ? std::string(kSceneRootId) : doc->root.id;
                if (seg == root_id)
                {
                    loc.found = true;
                    loc.file = current;
                    loc.entity_pointer = doc->root.pointer; // "/root"
                    loc.entity_value = &doc->root.value;
                    return loc;
                }
            }
            for (const AuthoredEntity& e : doc->entities)
                if (e.id == seg)
                {
                    loc.found = true;
                    loc.file = current;
                    loc.entity_pointer = e.pointer; // "/entities/<raw index>"
                    loc.entity_value = &e.value;
                    return loc;
                }
            loc.reason = "entity id `" + seg + "` is not authored in scene `" + current + "`";
            return loc;
        }

        // An interior segment: descend through the instance with this id.
        const SceneInstance* inst = nullptr;
        for (const SceneInstance& s : doc->instances)
            if (s.id == seg)
            {
                inst = &s;
                break;
            }
        if (inst == nullptr)
        {
            loc.reason = "instance id `" + seg + "` is not an instance of scene `" + current + "`";
            return loc;
        }
        current = inst->scene;
        doc = resolver.resolve(current);
        if (doc == nullptr)
        {
            loc.reason = "instanced scene `" + current + "` does not resolve to a known scene";
            return loc;
        }
    }
    loc.reason = "the id-path did not resolve to an entity"; // unreachable (loop returns)
    return loc;
}

// Walk `prefix` (a chain of instance ids) from `root_scene` to the scene it names; the scene an
// `--at-instance` override lands in. false (reason set) when a hop does not resolve.
[[nodiscard]] bool resolve_instance_scene(const std::string& root_scene,
                                          const std::vector<std::string>& prefix,
                                          const WriteResolver& resolver, std::string& out_scene,
                                          std::string& reason)
{
    std::string current = root_scene;
    const SceneDoc* doc = resolver.resolve(current);
    if (doc == nullptr)
    {
        reason = "scene `" + current + "` does not resolve to a known scene";
        return false;
    }
    for (const std::string& seg : prefix)
    {
        const SceneInstance* inst = nullptr;
        for (const SceneInstance& s : doc->instances)
            if (s.id == seg)
            {
                inst = &s;
                break;
            }
        if (inst == nullptr)
        {
            reason = "instance id `" + seg + "` is not an instance of scene `" + current + "`";
            return false;
        }
        current = inst->scene;
        doc = resolver.resolve(current);
        if (doc == nullptr)
        {
            reason = "instanced scene `" + current + "` does not resolve to a known scene";
            return false;
        }
    }
    out_scene = current;
    return true;
}

[[nodiscard]] bool is_prefix(const std::vector<std::string>& prefix,
                             const std::vector<std::string>& path)
{
    if (prefix.empty() || prefix.size() >= path.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (prefix[i] != path[i])
            return false;
    return true;
}

[[nodiscard]] WritePlan fail(std::string code, std::string message,
                             std::optional<std::string> pointer = std::nullopt)
{
    WritePlan plan;
    plan.ok = false;
    plan.error_code = std::move(code);
    plan.error_message = std::move(message);
    plan.error_pointer = std::move(pointer);
    return plan;
}

// Collect every scene path reachable from `root` (root + transitively instanced scenes), deduped so
// an instancing cycle terminates. Order is deterministic (DFS in authored instance order).
void collect_scenes(const std::string& scene, const WriteResolver& resolver,
                    std::set<std::string>& visited, std::vector<std::string>& order)
{
    if (!visited.insert(scene).second)
        return;
    const SceneDoc* doc = resolver.resolve(scene);
    if (doc == nullptr)
        return;
    order.push_back(scene);
    for (const SceneInstance& inst : doc->instances)
        collect_scenes(inst.scene, resolver, visited, order);
}

} // namespace

bool is_immutable_pointer(std::string_view pointer) noexcept
{
    std::vector<std::string> tokens;
    if (!parse_json_pointer(pointer, tokens) || tokens.empty())
        return false;
    return tokens.front() == "id" || tokens.front() == "$schema" || tokens.front() == "version";
}

WritePlan plan_write(const WriteRequest& request, const WriteResolver& resolver)
{
    if (request.id_path.empty())
        return fail("usage.invalid", "the composed-write id-path is empty — address an entity by "
                                     "its L-35 id-path (e.g. [instanceId, ..., entityId])");

    std::vector<std::string> pointer_tokens;
    if (!parse_json_pointer(request.pointer, pointer_tokens))
        return fail("usage.invalid",
                    "the field pointer `" + request.pointer + "` is not a valid JSON pointer");
    if (is_immutable_pointer(request.pointer))
        return fail("compose.immutable_pointer",
                    "`/" + pointer_tokens.front() +
                        "` is immutable under composition (L-37: stable ids and the schema header "
                        "survive re-derivation and are never written through composition)",
                    request.pointer);

    const SceneDoc* root_doc = resolver.resolve(request.root_scene);
    const JsonValue* root_tree = resolver.tree(request.root_scene);
    if (root_doc == nullptr || root_tree == nullptr)
        return fail("file.not_found",
                    "the root scene `" + request.root_scene + "` does not resolve to a known scene");

    // Resolve the addressing scene + the id-path relative to it (default: the root scene itself;
    // --at-instance: a mid-level instancing scene named by a strict prefix of the entity id-path).
    std::string addressing_scene = request.root_scene;
    std::vector<std::string> addressing_path = request.id_path;
    if (request.target == WriteTarget::at_instance)
    {
        if (!is_prefix(request.at_instance, request.id_path))
            return fail("usage.invalid",
                        "--at-instance <idPath> must be a strict, non-empty prefix of the entity "
                        "id-path (it names the mid-level instancing scene the override lands in)");
        std::string reason;
        if (!resolve_instance_scene(request.root_scene, request.at_instance, resolver,
                                    addressing_scene, reason))
            return fail("compose.write_target_not_found", "--at-instance path does not resolve: " +
                                                              reason);
        addressing_path.assign(request.id_path.begin() +
                                   static_cast<std::ptrdiff_t>(request.at_instance.size()),
                               request.id_path.end());
    }

    // Locate the defining template + the current template value at the pointer. For --edit-template
    // we walk the FULL id-path from the root; for an override we walk the addressing-relative path.
    const std::string walk_scene =
        request.target == WriteTarget::defining_template ? request.root_scene : addressing_scene;
    const std::vector<std::string>& walk_path =
        request.target == WriteTarget::defining_template ? request.id_path : addressing_path;
    const TemplateLocation tmpl = locate_template(walk_scene, walk_path, resolver);
    if (!tmpl.found)
        return fail("compose.write_target_not_found",
                    "the composed-write target does not resolve: " + tmpl.reason);

    const JsonValue* template_value = resolve_json_pointer(*tmpl.entity_value, request.pointer);

    // Direct template edit: --edit-template, OR the addressed entity is authored directly in the
    // addressing scene (a single-segment id-path — there is no outer scene to override it from, so
    // the change lands in the entity's authored value in place). Both write the defining file.
    const bool template_edit =
        request.target == WriteTarget::defining_template || addressing_path.size() == 1;
    if (template_edit)
    {
        const JsonValue* file_tree = resolver.tree(tmpl.file);
        if (file_tree == nullptr)
            return fail("file.not_found",
                        "the defining scene `" + tmpl.file + "` has no readable document tree");
        WritePlan plan;
        plan.document = *file_tree; // mutate a copy
        plan.pointer = tmpl.entity_pointer + request.pointer;
        if (!set_json_pointer(plan.document, plan.pointer, request.value))
            return fail("usage.invalid",
                        "the field pointer `" + request.pointer +
                            "` does not resolve inside the entity, or would grow an array / retype "
                            "a scalar (overrides never restructure containers)",
                        request.pointer);
        plan.ok = true;
        plan.file = tmpl.file;
        plan.target = request.target;
        plan.base_recorded = false;
        return plan;
    }

    // Otherwise an override entry in the addressing scene (outermost or --at-instance). The addressed
    // entity is nested (>= 2 segments): the override id-path is the addressing-relative path.
    const SceneDoc* addressing_doc = resolver.resolve(addressing_scene);
    const JsonValue* addressing_tree = resolver.tree(addressing_scene);
    if (addressing_doc == nullptr || addressing_tree == nullptr)
        return fail("file.not_found", "the addressing scene `" + addressing_scene +
                                          "` does not resolve to a writable scene");

    WritePlan plan;
    plan.document = *addressing_tree; // mutate a copy
    plan.file = addressing_scene;
    plan.target = request.target;

    // Idempotent update: reuse an existing field-override entry with the SAME id-path + pointer.
    std::size_t existing_index = 0;
    bool has_existing = false;
    for (const OverrideEntry& e : addressing_doc->overrides)
        if (e.kind == OverrideKind::field && e.path == addressing_path &&
            e.field_pointer == request.pointer)
        {
            if (override_index_of(e.pointer, existing_index))
                has_existing = true;
            break;
        }

    if (has_existing)
    {
        const std::string base = "/overrides/" + std::to_string(existing_index);
        if (!set_json_pointer(plan.document, base + "/value", request.value))
            return fail("internal.error",
                        "the existing override entry could not be updated in the scene tree");
        plan.pointer = base + "/value";
        if (template_value != nullptr && set_json_pointer(plan.document, base + "/base",
                                                          *template_value))
            plan.base_recorded = true;
    }
    else
    {
        JsonValue entry =
            make_override_entry(addressing_path, request.pointer, request.value, template_value);
        JsonValue& arr = overrides_array(plan.document);
        const std::size_t new_index = arr.elements.size();
        arr.elements.push_back(std::move(entry));
        plan.pointer = "/overrides/" + std::to_string(new_index) + "/value";
        plan.base_recorded = template_value != nullptr;
    }

    plan.ok = true;
    return plan;
}

std::vector<OverrideFinding> override_hygiene(std::string_view root_scene,
                                              const WriteResolver& resolver, HygieneKind kind,
                                              const ComposeLimits& limits)
{
    (void)limits; // reserved: hygiene walks the scene graph directly (cycle-terminated by `visited`)
    std::vector<OverrideFinding> findings;

    std::set<std::string> visited;
    std::vector<std::string> scenes;
    collect_scenes(std::string(root_scene), resolver, visited, scenes);

    for (const std::string& scene : scenes)
    {
        const SceneDoc* doc = resolver.resolve(scene);
        const JsonValue* tree = resolver.tree(scene);
        if (doc == nullptr || tree == nullptr)
            continue;
        for (const OverrideEntry& e : doc->overrides)
        {
            if (e.kind != OverrideKind::field)
                continue;
            const TemplateLocation tmpl = locate_template(scene, e.path, resolver);
            if (!tmpl.found)
                continue; // an orphan override is the flatten's compose.orphan_override, not hygiene
            const JsonValue* template_value =
                resolve_json_pointer(*tmpl.entity_value, e.field_pointer);
            if (template_value == nullptr)
                continue; // a NEW-field override: neither diverged (no base peer) nor redundant

            if (kind == HygieneKind::redundant)
            {
                if (canonical_equal(e.value, *template_value))
                    findings.push_back(OverrideFinding{
                        scene, e.pointer, e.path, e.field_pointer,
                        "the override value equals the current template value at this field — it is "
                        "redundant (it changes nothing); remove the override entry to drop it"});
                continue;
            }

            // diverged: the entry must carry a recorded `base` that no longer equals the template.
            const JsonValue* raw_entry = resolve_json_pointer(*tree, e.pointer);
            const JsonValue* base =
                raw_entry != nullptr ? schema::find_member(*raw_entry, "base") : nullptr;
            if (base == nullptr)
                continue; // hand-authored / pre-base override: not divergence-checkable
            if (!canonical_equal(*base, *template_value))
                findings.push_back(OverrideFinding{
                    scene, e.pointer, e.path, e.field_pointer,
                    "the override's recorded base no longer matches the current template value — the "
                    "template moved under this override (stale); re-review then re-set or remove it"});
        }
    }

    return findings;
}

} // namespace context::editor::compose
