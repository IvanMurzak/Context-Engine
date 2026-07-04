// Scene-composition flatten — see flatten.h.

#include "context/editor/compose/flatten.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace context::editor::compose
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// One override entry narrowed into the subtree currently being expanded: `remaining` is the tail
// of the entry's id-path still to be matched below this point. `level` / `owner_file` identify the
// contributing scene (0 = the flatten root).
struct NarrowedOverride
{
    const OverrideEntry* entry = nullptr;
    std::size_t level = 0;
    std::string owner_file;
    std::vector<std::string> remaining;
};

struct FlattenState
{
    const SceneResolver* resolver = nullptr;
    ComposeLimits limits;
    ComposedScene out;
    std::map<std::string, std::size_t> fan_in; // scene path -> instancing count in this flatten
    std::set<std::string> fan_in_reported;     // fan-in advisory emitted once per scene file
    std::set<std::string> docs_reported;       // intra-file findings surfaced once per file
    std::set<const OverrideEntry*> registered; // entries known to the orphan sweep
    std::set<const OverrideEntry*> consumed;   // entries that resolved or were suppressed
    std::vector<NarrowedOverride> all_entries; // registration order, for the orphan sweep
    bool budget_exhausted = false;             // max_entities tripped (diagnosed once)
};

void diagnose(FlattenState& s, std::string code, std::string message, const std::string& file,
              std::string pointer, bool blocking)
{
    ComposeDiagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.file = file;
    d.pointer = std::move(pointer);
    d.blocking = blocking;
    if (blocking)
        s.out.ok = false;
    s.out.diagnostics.push_back(std::move(d));
}

[[nodiscard]] std::string joined_path(const std::vector<std::string>& segments)
{
    std::string out;
    for (const std::string& seg : segments)
    {
        if (!out.empty())
            out.push_back('/');
        out += seg;
    }
    return out;
}

// Compose one entity: honor structural removes, apply the matching field overrides with
// innermost-out precedence (the OUTERMOST instancing scene — the LOWEST level — wins), and record
// winning-value-first provenance chains. `baseline` is the defining value; `baseline_source`
// describes it for the chain tail (template_value for authored entities, override_value for
// structural adds, whose defining value IS an override entry).
void compose_entity(FlattenState& s, const JsonValue& baseline, const std::string& entity_id,
                    const std::string& defining_file, const std::string& defining_pointer,
                    std::size_t defining_level, ProvenanceEntry::Source baseline_source,
                    const std::vector<std::string>& prefix,
                    const std::vector<NarrowedOverride>& scope)
{
    // This entity's contributors: entries whose remaining id-path is exactly [entity_id].
    std::vector<const NarrowedOverride*> matching;
    for (const NarrowedOverride& n : scope)
        if (n.remaining.size() == 1 && n.remaining.front() == entity_id &&
            n.entry->kind != OverrideKind::add)
            matching.push_back(&n);

    // Structural remove: removal at ANY level removes the entity. An outer FIELD override on a
    // removed entity never resurrects it — it stays unconsumed and surfaces as an orphan.
    bool removed = false;
    for (const NarrowedOverride* n : matching)
    {
        if (n->entry->kind == OverrideKind::remove)
        {
            s.consumed.insert(n->entry);
            removed = true;
        }
    }
    if (removed)
        return;

    // Output budget (R-FILE-011(e)): a dense fan-in/diamond graph cannot run away with memory.
    if (s.out.entities.size() >= s.limits.max_entities)
    {
        if (!s.budget_exhausted)
        {
            s.budget_exhausted = true;
            diagnose(s, "compose.too_many_entities",
                     "flatten output exceeded the composed-entity budget (" +
                         std::to_string(s.limits.max_entities) +
                         ") — expansion stopped (R-FILE-011(e): bounded composition)",
                     s.out.root_path, "", true);
        }
        return;
    }

    ComposedEntity entity;
    entity.id_path = prefix;
    entity.id_path.push_back(entity_id);
    entity.identity_hash = identity_hash_of(s.out.root_path, entity.id_path);
    entity.value = baseline;
    entity.template_file = defining_file;
    entity.template_pointer = defining_pointer;
    entity.template_level = defining_level;

    // Field overrides grouped per pointer; contributors sorted by level ascending (outermost
    // first). The winner (lowest level) is applied; every contributor joins the chain.
    std::map<std::string, std::vector<const NarrowedOverride*>> by_pointer;
    for (const NarrowedOverride* n : matching)
        if (n->entry->kind == OverrideKind::field)
            by_pointer[n->entry->field_pointer].push_back(n);

    for (auto& [pointer, contributors] : by_pointer)
    {
        std::stable_sort(contributors.begin(), contributors.end(),
                         [](const NarrowedOverride* a, const NarrowedOverride* b)
                         { return a->level < b->level; });
        const NarrowedOverride* winner = contributors.front();
        if (!set_json_pointer(entity.value, pointer, winner->entry->value))
        {
            // Structurally inapplicable (array growth / scalar mid-path): the whole group is
            // orphaned — the final sweep reports each unconsumed entry.
            continue;
        }
        FieldProvenance provenance;
        provenance.pointer = pointer;
        for (const NarrowedOverride* n : contributors)
        {
            s.consumed.insert(n->entry);
            ProvenanceEntry link;
            link.source = ProvenanceEntry::Source::override_value;
            link.file = n->owner_file;
            link.pointer = n->entry->pointer + "/value";
            link.level = n->level;
            provenance.chain.push_back(std::move(link));
        }
        // The defining value closes the chain when the baseline actually authors this pointer.
        if (resolve_json_pointer(baseline, pointer) != nullptr)
        {
            ProvenanceEntry link;
            link.source = baseline_source;
            link.file = defining_file;
            link.pointer = defining_pointer + pointer;
            link.level = defining_level;
            provenance.chain.push_back(std::move(link));
        }
        entity.field_provenance.push_back(std::move(provenance));
    }

    s.out.entities.push_back(std::move(entity));
}

// Expand one scene into the flatten output. `prefix` is the id-path down to (excluding) this
// scene's own collections; `stack` is the instancing chain for cycle detection; `scope` carries
// override entries narrowed into this subtree from every outer level; `pending_adds` are the
// structural adds whose id-path resolved exactly to this subtree.
void expand(FlattenState& s, const std::string& scene_path, const SceneDoc& doc,
            const std::vector<std::string>& prefix, std::size_t depth,
            std::vector<std::string>& stack, std::vector<NarrowedOverride> scope,
            const std::vector<NarrowedOverride>& pending_adds)
{
    if (s.budget_exhausted)
        return;

    // Surface this scene's intra-file findings — once per FILE per flatten (fan-in re-expansions
    // must not multiply them).
    if (s.docs_reported.insert(scene_path).second)
    {
        for (const ComposeDiagnostic& d : doc.diagnostics)
        {
            if (d.blocking)
                s.out.ok = false;
            s.out.diagnostics.push_back(d);
        }
    }

    // Register this scene's own override entries: they address its instances ([instanceId, …]),
    // narrowing at the hops below. Registered once per entry for the orphan sweep; consumption in
    // ANY expansion of this scene counts (the entry is part of the template's definition).
    for (const OverrideEntry& e : doc.overrides)
    {
        NarrowedOverride n;
        n.entry = &e;
        n.level = depth;
        n.owner_file = scene_path;
        n.remaining = e.path;
        if (s.registered.insert(&e).second)
            s.all_entries.push_back(n);
        scope.push_back(std::move(n));
    }

    // --- the scene-root entity (L-35 scene-level state) ------------------------------------------
    // The flatten root's root entity always composes; an instanced sub-scene's root is inert by
    // default and composes only with the `composable` opt-in.
    if (doc.root.present && (depth == 0 || doc.root.composable))
    {
        const std::string root_id = doc.root.id.empty() ? std::string(kSceneRootId) : doc.root.id;
        compose_entity(s, doc.root.value, root_id, scene_path, doc.root.pointer, depth,
                       ProvenanceEntry::Source::template_value, prefix, scope);
    }

    // --- authored entities (id-keyed, authored order) --------------------------------------------
    for (const AuthoredEntity& e : doc.entities)
        compose_entity(s, e.value, e.id, scene_path, e.pointer, depth,
                       ProvenanceEntry::Source::template_value, prefix, scope);

    // --- structural adds targeting THIS subtree ---------------------------------------------------
    // An added entity behaves like an entity defined here, except its baseline (and chain tail)
    // is the override entry itself. Its id joins this scene's id space for this expansion.
    std::set<std::string> local_ids;
    for (const AuthoredEntity& e : doc.entities)
        local_ids.insert(e.id);
    for (const SceneInstance& inst : doc.instances)
        local_ids.insert(inst.id);
    if (doc.root.present && !doc.root.id.empty())
        local_ids.insert(doc.root.id);

    for (const NarrowedOverride& add : pending_adds)
    {
        s.consumed.insert(add.entry);
        const JsonValue* id = schema::find_member(add.entry->value, "id");
        if (id == nullptr || id->type != JsonValue::Type::string || !is_stable_id(id->string_value))
        {
            diagnose(s, "compose.missing_id",
                     "structural add ignored: the added entity carries no valid stable `id` "
                     "(L-33)",
                     add.owner_file, add.entry->pointer + "/add", false);
            continue;
        }
        if (!local_ids.insert(id->string_value).second)
        {
            diagnose(s, "compose.duplicate_id",
                     "structural add ignored: id `" + id->string_value +
                         "` already exists in the target scene's id space (L-33/R-FILE-012)",
                     add.owner_file, add.entry->pointer + "/add/id", true);
            continue;
        }
        compose_entity(s, add.entry->value, id->string_value, add.owner_file,
                       add.entry->pointer + "/add", add.level,
                       ProvenanceEntry::Source::override_value, prefix, scope);
    }

    // --- instances (depth-first, authored order) --------------------------------------------------
    for (const SceneInstance& inst : doc.instances)
    {
        // Narrow the scope to this instance's subtree.
        std::vector<NarrowedOverride> child_scope;
        std::vector<NarrowedOverride> child_adds;
        bool subtree_removed = false;
        for (const NarrowedOverride& n : scope)
        {
            if (n.remaining.empty() || n.remaining.front() != inst.id)
                continue;
            NarrowedOverride narrowed = n;
            narrowed.remaining.erase(narrowed.remaining.begin());
            if (narrowed.remaining.empty())
            {
                switch (n.entry->kind)
                {
                case OverrideKind::remove:
                    // Structural remove of the WHOLE instance subtree (any level suffices).
                    s.consumed.insert(n.entry);
                    subtree_removed = true;
                    break;
                case OverrideKind::add:
                    // The add's id-path resolved to this subtree; the entity joins the child.
                    child_adds.push_back(std::move(narrowed));
                    break;
                case OverrideKind::field:
                    // A field override cannot stop at an instance id — it never resolves and
                    // surfaces in the orphan sweep.
                    break;
                }
                continue;
            }
            child_scope.push_back(std::move(narrowed));
        }
        if (subtree_removed)
        {
            // Entries narrowed into a removed subtree are moot, not orphans.
            for (const NarrowedOverride& n : child_scope)
                s.consumed.insert(n.entry);
            for (const NarrowedOverride& n : child_adds)
                s.consumed.insert(n.entry);
            continue;
        }

        // Suppression closure for a subtree this pass cannot expand (cycle / depth / missing
        // scene): its narrowed entries are covered by the blocking diagnostic, not orphans.
        const auto suppress = [&s, &child_scope, &child_adds]()
        {
            for (const NarrowedOverride& n : child_scope)
                s.consumed.insert(n.entry);
            for (const NarrowedOverride& n : child_adds)
                s.consumed.insert(n.entry);
        };

        if (std::find(stack.begin(), stack.end(), inst.scene) != stack.end())
        {
            diagnose(s, "compose.cycle",
                     "instancing cycle: `" + inst.scene +
                         "` is already on the expansion chain — the instance is not expanded",
                     scene_path, inst.pointer, true);
            suppress();
            continue;
        }
        if (depth + 1 > s.limits.max_depth)
        {
            diagnose(s, "compose.depth_exceeded",
                     "composition nesting depth exceeds the cap (" +
                         std::to_string(s.limits.max_depth) +
                         ") — the instance is not expanded (R-FILE-011(e))",
                     scene_path, inst.pointer, true);
            suppress();
            continue;
        }
        const SceneDoc* child = s.resolver->resolve(inst.scene);
        if (child == nullptr)
        {
            diagnose(s, "compose.missing_scene",
                     "instanced scene `" + inst.scene + "` does not resolve to a known scene",
                     scene_path, inst.pointer, true);
            suppress();
            continue;
        }

        // Fan-in tally (advisory, once per scene file per flatten — R-FILE-011(e)).
        const std::size_t count = ++s.fan_in[inst.scene];
        if (count >= s.limits.fan_in_threshold && s.fan_in_reported.insert(inst.scene).second)
        {
            diagnose(s, "compose.fan_in",
                     "scene `" + inst.scene + "` is instanced " + std::to_string(count) +
                         "+ times in one flatten (threshold " +
                         std::to_string(s.limits.fan_in_threshold) +
                         ") — consider splitting (R-FILE-011(e))",
                     scene_path, inst.pointer, false);
        }

        std::vector<std::string> child_prefix = prefix;
        child_prefix.push_back(inst.id);
        stack.push_back(inst.scene);
        expand(s, inst.scene, *child, child_prefix, depth + 1, stack, std::move(child_scope),
               child_adds);
        stack.pop_back();
    }
}

} // namespace

ComposedScene flatten(std::string_view root_path, const SceneResolver& resolver,
                      const ComposeLimits& limits)
{
    FlattenState s;
    s.resolver = &resolver;
    s.limits = limits;
    s.out.root_path = std::string(root_path);

    const SceneDoc* root = resolver.resolve(root_path);
    if (root == nullptr)
    {
        diagnose(s, "compose.missing_scene",
                 "flatten root `" + s.out.root_path + "` does not resolve to a known scene",
                 s.out.root_path, "", true);
        return std::move(s.out);
    }

    std::vector<std::string> stack{s.out.root_path};
    expand(s, s.out.root_path, *root, {}, 0, stack, {}, {});

    // Orphan sweep (the L-37 wording: unmappable override paths become orphan diagnostics,
    // excluded from flatten). Skipped when the entity budget tripped — a truncated expansion
    // cannot tell an orphan from a not-yet-reached target.
    if (!s.budget_exhausted)
    {
        for (const NarrowedOverride& n : s.all_entries)
        {
            if (s.consumed.count(n.entry) != 0)
                continue;
            diagnose(s, "compose.orphan_override",
                     "override path [" + joined_path(n.entry->path) +
                         "] does not resolve to a composed target (unknown instance/entity id, a "
                         "removed entity, or a structurally inapplicable pointer)",
                     n.owner_file, n.entry->pointer, false);
        }
    }

    return std::move(s.out);
}

std::vector<ProvenanceEntry> provenance_for(const ComposedEntity& entity, std::string_view pointer)
{
    for (const FieldProvenance& f : entity.field_provenance)
        if (f.pointer == pointer)
            return f.chain;
    ProvenanceEntry link;
    link.source = ProvenanceEntry::Source::template_value;
    link.file = entity.template_file;
    link.pointer = entity.template_pointer + std::string(pointer);
    link.level = entity.template_level;
    return {link};
}

std::uint64_t identity_hash_of(std::string_view root_path, const std::vector<std::string>& id_path)
{
    // Length-prefixing the root path keeps the encoding injective even when the path itself
    // contains the 0x1F separator (a legal filename byte): decoding reads the length, takes that
    // many bytes as the path, and splits the rest on 0x1F — id-path segments (stable ids / $root)
    // never contain it. Without the prefix, ("A\x1fB", ["C"]) and ("A", ["B", "C"]) would collide.
    std::string joined = std::to_string(root_path.size());
    joined.reserve(joined.size() + root_path.size() + id_path.size() * 17 + 1);
    joined.push_back('\x1f');
    joined.append(root_path);
    for (const std::string& seg : id_path)
    {
        joined.push_back('\x1f');
        joined.append(seg);
    }
    return serializer::canonical_hash_of(joined);
}

namespace
{

[[nodiscard]] const char* source_name(ProvenanceEntry::Source source)
{
    switch (source)
    {
    case ProvenanceEntry::Source::schema_default:
        return "schemaDefault";
    case ProvenanceEntry::Source::template_value:
        return "template";
    case ProvenanceEntry::Source::override_value:
        return "override";
    }
    return "template";
}

[[nodiscard]] JsonValue json_string(std::string_view v)
{
    JsonValue out;
    out.type = JsonValue::Type::string;
    out.string_value = std::string(v);
    return out;
}

[[nodiscard]] JsonValue json_bool(bool v)
{
    JsonValue out;
    out.type = JsonValue::Type::boolean;
    out.boolean_value = v;
    return out;
}

[[nodiscard]] JsonValue json_uint(std::uint64_t v)
{
    JsonValue out;
    out.type = JsonValue::Type::unsigned_integer;
    out.uint_value = v;
    return out;
}

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    JsonMember m;
    m.key = std::string(key);
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

[[nodiscard]] JsonValue provenance_tree(const std::vector<ProvenanceEntry>& chain)
{
    JsonValue array;
    array.type = JsonValue::Type::array;
    for (const ProvenanceEntry& link : chain)
    {
        JsonValue entry;
        entry.type = JsonValue::Type::object;
        set_member(entry, "file", json_string(link.file));
        set_member(entry, "level", json_uint(static_cast<std::uint64_t>(link.level)));
        set_member(entry, "pointer", json_string(link.pointer));
        set_member(entry, "source", json_string(source_name(link.source)));
        array.elements.push_back(std::move(entry));
    }
    return array;
}

} // namespace

std::string provenance_json(const std::vector<ProvenanceEntry>& chain)
{
    // Built from parsed strings (already valid UTF-8) and unsigned integers — no doubles, so
    // canonical serialization cannot fail.
    std::string out;
    (void)serializer::serialize_canonical(provenance_tree(chain), out);
    return out;
}

std::string composed_scene_json(const ComposedScene& scene)
{
    JsonValue root;
    root.type = JsonValue::Type::object;
    set_member(root, "rootScene", json_string(scene.root_path));
    set_member(root, "ok", json_bool(scene.ok));

    JsonValue entities;
    entities.type = JsonValue::Type::array;
    for (const ComposedEntity& e : scene.entities)
    {
        JsonValue entity;
        entity.type = JsonValue::Type::object;
        JsonValue id_path;
        id_path.type = JsonValue::Type::array;
        for (const std::string& seg : e.id_path)
            id_path.elements.push_back(json_string(seg));
        set_member(entity, "idPath", std::move(id_path));
        set_member(entity, "identityHash", json_string(format_stable_id(e.identity_hash)));
        set_member(entity, "templateFile", json_string(e.template_file));
        set_member(entity, "templatePointer", json_string(e.template_pointer));
        set_member(entity, "value", e.value);
        JsonValue provenance;
        provenance.type = JsonValue::Type::array;
        for (const FieldProvenance& f : e.field_provenance)
        {
            JsonValue field;
            field.type = JsonValue::Type::object;
            set_member(field, "chain", provenance_tree(f.chain));
            set_member(field, "pointer", json_string(f.pointer));
            provenance.elements.push_back(std::move(field));
        }
        set_member(entity, "provenance", std::move(provenance));
        entities.elements.push_back(std::move(entity));
    }
    set_member(root, "entities", std::move(entities));

    JsonValue diagnostics;
    diagnostics.type = JsonValue::Type::array;
    for (const ComposeDiagnostic& d : scene.diagnostics)
    {
        JsonValue diag;
        diag.type = JsonValue::Type::object;
        set_member(diag, "blocking", json_bool(d.blocking));
        set_member(diag, "code", json_string(d.code));
        set_member(diag, "file", json_string(d.file));
        set_member(diag, "message", json_string(d.message));
        set_member(diag, "pointer", json_string(d.pointer));
        diagnostics.elements.push_back(std::move(diag));
    }
    set_member(root, "diagnostics", std::move(diagnostics));

    std::string out;
    (void)serializer::serialize_canonical(root, out);
    return out;
}

} // namespace context::editor::compose
