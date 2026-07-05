// Structural three-way merge implementation (see three_way_merge.h).

#include "context/editor/merge/three_way_merge.h"

#include "context/editor/serializer/canonical.h"

#include "pointer_format.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace context::editor::merge
{

using serializer::JsonValue;

// --- conflict-class metadata ------------------------------------------------------------------

const char* to_string(ConflictClass klass) noexcept
{
    switch (klass)
    {
    case ConflictClass::field:
        return "field";
    case ConflictClass::id_add_add:
        return "id_add_add";
    case ConflictClass::delete_modify:
        return "delete_modify";
    case ConflictClass::binary_sidecar:
        return "binary_sidecar";
    case ConflictClass::meta_guid:
        return "meta_guid";
    case ConflictClass::newer_stamped:
        return "newer_stamped";
    }
    return "field";
}

const char* catalog_code(ConflictClass klass) noexcept
{
    switch (klass)
    {
    case ConflictClass::field:
    case ConflictClass::delete_modify:
        return "merge.conflict";
    case ConflictClass::id_add_add:
        return "merge.id_conflict";
    case ConflictClass::binary_sidecar:
        return "merge.binary_sidecar";
    case ConflictClass::meta_guid:
        return "merge.meta_guid";
    case ConflictClass::newer_stamped:
        return "merge.newer_stamped";
    }
    return "merge.conflict";
}

bool is_whole_file(ConflictClass klass) noexcept
{
    return klass == ConflictClass::binary_sidecar || klass == ConflictClass::meta_guid ||
           klass == ConflictClass::newer_stamped;
}

// --- structural equality ----------------------------------------------------------------------

bool json_equal(const JsonValue& a, const JsonValue& b) noexcept
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case JsonValue::Type::null_value:
        return true;
    case JsonValue::Type::boolean:
        return a.boolean_value == b.boolean_value;
    case JsonValue::Type::integer:
        return a.int_value == b.int_value;
    case JsonValue::Type::unsigned_integer:
        return a.uint_value == b.uint_value;
    case JsonValue::Type::number:
        return a.number_value == b.number_value;
    case JsonValue::Type::string:
        return a.string_value == b.string_value;
    case JsonValue::Type::array:
        if (a.elements.size() != b.elements.size())
            return false;
        for (std::size_t i = 0; i < a.elements.size(); ++i)
            if (!json_equal(a.elements[i], b.elements[i]))
                return false;
        return true;
    case JsonValue::Type::object:
    {
        // Order-insensitive: keys are unique (the parser enforces it), so equal member COUNT plus
        // a matching key/value for each of a's members is set-equality.
        if (a.members.size() != b.members.size())
            return false;
        for (const serializer::JsonMember& ma : a.members)
        {
            const JsonValue* mb = nullptr;
            for (const serializer::JsonMember& cand : b.members)
                if (cand.key == ma.key)
                {
                    mb = &cand.value;
                    break;
                }
            if (mb == nullptr || !json_equal(ma.value, *mb))
                return false;
        }
        return true;
    }
    }
    return false;
}

namespace
{

// Find an object member by key. nullptr when absent or `v` is not an object.
const JsonValue* member(const JsonValue& v, const std::string& key) noexcept
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The string `id` member of an object, or empty when absent / not a string.
std::string id_of(const JsonValue& v)
{
    const JsonValue* id = member(v, "id");
    if (id != nullptr && id->type == JsonValue::Type::string)
        return id->string_value;
    return std::string();
}

// An absent common ancestor. merge_command seeds a missing ancestor (an add/add on both sides) as an
// EMPTY object because the structural add/add path needs base.type==object; a genuine JSON null is
// likewise "no value". Treating both as absent keeps the R-CLI-008 conflict envelope exact — an absent
// `base` is OMITTED, never emitted as {} (a real whole-file/field ancestor is never an empty object).
// Shared by whole_file() and merge_value()'s field-conflict emitter so both omit an absent base identically.
bool base_is_absent_ancestor(const JsonValue& base) noexcept
{
    return base.type == JsonValue::Type::null_value ||
           (base.type == JsonValue::Type::object && base.members.empty());
}

using detail::append_index;
using detail::append_token;

void set_member(JsonValue& object, const std::string& key, JsonValue value)
{
    serializer::JsonMember m;
    m.key = key;
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

// Forward decl: the recursive reconciler.
JsonValue merge_value(const JsonValue& base, const JsonValue& ours, const JsonValue& theirs,
                      const std::string& path, std::vector<Conflict>& conflicts);

// Merge three objects member-by-member (standard 3-way per key).
JsonValue merge_object(const JsonValue& base, const JsonValue& ours, const JsonValue& theirs,
                       const std::string& path, std::vector<Conflict>& conflicts)
{
    JsonValue out;
    out.type = JsonValue::Type::object;

    // Key visitation order: ours members, then theirs-only, then base-only. Output member order is
    // irrelevant (the canonical writer sorts keys) but a stable order keeps conflict paths deterministic.
    std::vector<std::string> keys;
    auto add_keys = [&keys](const JsonValue& v) {
        if (v.type != JsonValue::Type::object)
            return;
        for (const serializer::JsonMember& m : v.members)
            if (std::find(keys.begin(), keys.end(), m.key) == keys.end())
                keys.push_back(m.key);
    };
    add_keys(ours);
    add_keys(theirs);
    add_keys(base);

    for (const std::string& k : keys)
    {
        const JsonValue* bp = member(base, k);
        const JsonValue* op = member(ours, k);
        const JsonValue* tp = member(theirs, k);
        const std::string child = append_token(path, k);

        if (op != nullptr && tp != nullptr && bp != nullptr)
        {
            set_member(out, k, merge_value(*bp, *op, *tp, child, conflicts));
        }
        else if (op != nullptr && tp != nullptr) // added on both (no base)
        {
            if (json_equal(*op, *tp))
            {
                set_member(out, k, *op);
            }
            else
            {
                Conflict c;
                c.path = child;
                c.klass = ConflictClass::field;
                c.ours = *op;
                c.theirs = *tp;
                conflicts.push_back(std::move(c));
                set_member(out, k, *op); // deterministic ours placeholder
            }
        }
        else if (op != nullptr && bp != nullptr) // theirs removed it
        {
            if (json_equal(*bp, *op))
            {
                // ours untouched, theirs removed => take the removal (omit the member).
            }
            else
            {
                Conflict c; // ours modified, theirs deleted
                c.path = child;
                c.klass = ConflictClass::delete_modify;
                c.base = *bp;
                c.ours = *op;
                conflicts.push_back(std::move(c));
                set_member(out, k, *op); // keep ours's modification as the placeholder
            }
        }
        else if (tp != nullptr && bp != nullptr) // ours removed it
        {
            if (json_equal(*bp, *tp))
            {
                // theirs untouched, ours removed => take the removal (omit the member).
            }
            else
            {
                Conflict c; // theirs modified, ours deleted
                c.path = child;
                c.klass = ConflictClass::delete_modify;
                c.base = *bp;
                c.theirs = *tp;
                conflicts.push_back(std::move(c));
                // ours removed it => the placeholder is the removal (omit the member).
            }
        }
        else if (op != nullptr) // added by ours only
        {
            set_member(out, k, *op);
        }
        else if (tp != nullptr) // added by theirs only
        {
            set_member(out, k, *tp);
        }
        // else: present only in base, removed on both => omit.
    }
    return out;
}

// A decision about one id in an id-keyed array merge, resolved in a first pass so the merged array's
// order (and therefore each element's final index, used in conflict paths) is known before the
// second pass computes element values.
struct IdDecision
{
    std::string id;
    const JsonValue* base = nullptr;
    const JsonValue* ours = nullptr;
    const JsonValue* theirs = nullptr;
    bool include = true; // false => the element is removed from the merged array
};

// Order-bearing index: the array's (id, element) pairs in array order. ours/theirs iterate this to
// drive the merged array's order; is_id_keyed_array guarantees each side's ids are unique.
std::vector<std::pair<std::string, const JsonValue*>> id_index(const JsonValue& array)
{
    std::vector<std::pair<std::string, const JsonValue*>> out;
    if (array.type != JsonValue::Type::array)
        return out;
    for (const JsonValue& el : array.elements)
    {
        std::string id = id_of(el);
        if (!id.empty())
            out.emplace_back(std::move(id), &el);
    }
    return out;
}

// id -> element hash index for O(1) lookup — keeps a large id-keyed array's merge linear, not
// quadratic. First occurrence wins, mirroring the historical linear scan; only `base` can carry a
// duplicate id (a pre-existing ancestor), since is_id_keyed_array rejects a duplicate-carrying side.
std::unordered_map<std::string, const JsonValue*> id_map(const JsonValue& array)
{
    std::unordered_map<std::string, const JsonValue*> out;
    if (array.type != JsonValue::Type::array)
        return out;
    for (const JsonValue& el : array.elements)
    {
        std::string id = id_of(el);
        if (!id.empty())
            out.emplace(std::move(id), &el); // emplace keeps the FIRST occurrence
    }
    return out;
}

const JsonValue* lookup(const std::unordered_map<std::string, const JsonValue*>& index,
                        const std::string& id)
{
    const auto it = index.find(id);
    return it != index.end() ? it->second : nullptr;
}

// Merge two-or-three id-keyed arrays by id identity (L-33). Order policy (v1, documented): ours's
// surviving order, then theirs-only additions appended in theirs order — deterministic and
// corruption-free under reorders; pure reorder divergence is not itself flagged as a conflict.
JsonValue merge_id_array(const JsonValue& base, const JsonValue& ours, const JsonValue& theirs,
                         const std::string& path, std::vector<Conflict>& conflicts)
{
    const auto ours_ix = id_index(ours);     // order-bearing: drives the merged array order
    const auto theirs_ix = id_index(theirs); // order-bearing: theirs-only adds append in this order
    const auto base_map = id_map(base);      // lookup-only
    std::unordered_map<std::string, const JsonValue*> theirs_map;
    theirs_map.reserve(theirs_ix.size());
    for (const auto& [id, el] : theirs_ix)
        theirs_map.emplace(id, el);

    // Pass 1: decide membership + order. O(1) lookups + a planned-id set keep a large id-keyed array
    // (thousands of entities) linear rather than quadratic.
    std::vector<IdDecision> plan;
    std::unordered_set<std::string> planned;

    for (const auto& [id, ours_el] : ours_ix)
    {
        IdDecision d;
        d.id = id;
        d.base = lookup(base_map, id);
        d.ours = ours_el;
        d.theirs = lookup(theirs_map, id);
        // ours present; keep unless theirs removed a base element ours did not touch.
        if (d.theirs == nullptr && d.base != nullptr && json_equal(*d.base, *d.ours))
            d.include = false; // theirs removed it, ours untouched => removal wins
        planned.insert(id);
        plan.push_back(std::move(d));
    }
    for (const auto& [id, theirs_el] : theirs_ix)
    {
        if (planned.count(id) != 0)
            continue; // already planned from ours
        IdDecision d;
        d.id = id;
        d.base = lookup(base_map, id);
        d.ours = nullptr;
        d.theirs = theirs_el;
        if (d.base != nullptr && json_equal(*d.base, *d.theirs))
            d.include = false; // ours removed it, theirs untouched => removal wins
        plan.push_back(std::move(d));
    }

    // Pass 2: compute element values at their final indices, collecting conflicts.
    JsonValue out;
    out.type = JsonValue::Type::array;
    for (const IdDecision& d : plan)
    {
        // include==false is only ever set for a confirmed-clean removal (one side removed an element
        // the other left byte-identical to base), so it contributes nothing to the merged array.
        if (!d.include)
            continue;

        const std::size_t index = out.elements.size();
        const std::string child = append_index(path, index);

        if (d.ours != nullptr && d.theirs != nullptr && d.base != nullptr)
        {
            const std::size_t before = conflicts.size();
            out.elements.push_back(merge_value(*d.base, *d.ours, *d.theirs, child, conflicts));
            // Propagate the enclosing element's stable id to any field conflict nested inside it
            // (id-path granularity for override entries — R-FILE-012(a)); an inner id-keyed array
            // already stamped its own inner id, so only fill the ones still unannotated.
            for (std::size_t k = before; k < conflicts.size(); ++k)
                if (conflicts[k].id.empty())
                    conflicts[k].id = d.id;
        }
        else if (d.ours != nullptr && d.theirs != nullptr) // same id added on BOTH sides (no base)
        {
            // R-FILE-012(b): NEVER silently unified — a structural conflict even when identical.
            Conflict c;
            c.path = child;
            c.klass = ConflictClass::id_add_add;
            c.id = d.id;
            c.ours = *d.ours;
            c.theirs = *d.theirs;
            conflicts.push_back(std::move(c));
            out.elements.push_back(*d.ours); // deterministic ours placeholder
        }
        else if (d.ours != nullptr && d.base != nullptr) // theirs removed; ours present
        {
            if (json_equal(*d.base, *d.ours))
            {
                out.elements.push_back(*d.ours); // both effectively unchanged (shouldn't reach: include==false)
            }
            else
            {
                Conflict c; // ours modified, theirs deleted
                c.path = child;
                c.klass = ConflictClass::delete_modify;
                c.id = d.id;
                c.base = *d.base;
                c.ours = *d.ours;
                conflicts.push_back(std::move(c));
                out.elements.push_back(*d.ours);
            }
        }
        else if (d.ours != nullptr) // ours-only add
        {
            out.elements.push_back(*d.ours);
        }
        else if (d.theirs != nullptr && d.base != nullptr) // ours removed; theirs present
        {
            // ours removed it; only surface a conflict when theirs modified it (else it stays removed).
            if (!json_equal(*d.base, *d.theirs))
            {
                Conflict c; // theirs modified, ours deleted
                c.path = child;
                c.klass = ConflictClass::delete_modify;
                c.id = d.id;
                c.base = *d.base;
                c.theirs = *d.theirs;
                conflicts.push_back(std::move(c));
                out.elements.push_back(*d.theirs); // keep theirs's modification as the placeholder
            }
        }
        else if (d.theirs != nullptr) // theirs-only add
        {
            out.elements.push_back(*d.theirs);
        }
    }
    return out;
}

JsonValue merge_value(const JsonValue& base, const JsonValue& ours, const JsonValue& theirs,
                      const std::string& path, std::vector<Conflict>& conflicts)
{
    if (json_equal(ours, theirs))
        return ours; // both sides agree (both unchanged, or both made the same change)
    if (json_equal(base, ours))
        return theirs; // only theirs changed
    if (json_equal(base, theirs))
        return ours; // only ours changed

    // Both sides changed differently.
    if (base.type == JsonValue::Type::object && ours.type == JsonValue::Type::object &&
        theirs.type == JsonValue::Type::object)
        return merge_object(base, ours, theirs, path, conflicts);

    if (is_id_keyed_array(ours) && is_id_keyed_array(theirs))
        return merge_id_array(base, ours, theirs, path, conflicts);

    // A leaf / opaque-array / type-mismatch divergence: an unmergeable field conflict.
    Conflict c;
    c.path = path;
    c.klass = ConflictClass::field;
    // Omit `base` for a no-ancestor divergence (e.g. both sides created this file with incompatible
    // root shapes, so merge_command seeded the absent ancestor as the empty-object sentinel): an absent
    // side is omitted from the envelope, never emitted as {} (mirrors whole_file()).
    if (!base_is_absent_ancestor(base))
        c.base = base;
    c.ours = ours;
    c.theirs = theirs;
    conflicts.push_back(std::move(c));
    return ours; // deterministic ours placeholder (valid JSON, never a text marker)
}

// Read the document's kind ("$schema"), top-level "version", and per-component schemaVersion stamps,
// then report whether any stamp EXCEEDS the installed floor (the L-37 newer-stamped whole-file
// trigger). An unlisted kind/component is not judged (packages register incrementally).
bool exceeds_floor(const JsonValue& doc, const SchemaFloor& floor)
{
    std::vector<serializer::Diagnostic> ignored;
    const serializer::DocumentHeader header = serializer::read_document_header(doc, ignored);

    const JsonValue* schema = member(doc, "$schema");
    if (schema != nullptr && schema->type == JsonValue::Type::string && header.has_version)
    {
        const auto it = floor.kind_versions.find(schema->string_value);
        if (it != floor.kind_versions.end() && header.version > it->second)
            return true;
    }
    if (header.has_component_versions)
    {
        for (const auto& [type, version] : header.component_versions)
        {
            const auto it = floor.component_versions.find(type);
            if (it != floor.component_versions.end() && version > it->second)
                return true;
        }
    }
    return false;
}

MergeResult whole_file(ConflictClass klass, const JsonValue& base, const JsonValue& ours,
                       const JsonValue& theirs)
{
    MergeResult result;
    result.clean = false;
    result.whole_file = true;
    result.merged = ours; // whole-file classes default the working tree to ours until resolved
    Conflict c;
    c.path = "";
    c.klass = klass;
    // Omit `base` when there is no real ancestor (R-CLI-008: an absent side is omitted, never {}).
    // A real whole-file-class ancestor (a .meta GUID doc, a schema-stamped payload) is never an empty
    // object, so treating the add/add empty-object sentinel as absent keeps the conflict envelope
    // exact; because this only affects emission, that structural add/add sentinel is left untouched.
    if (!base_is_absent_ancestor(base))
        c.base = base;
    c.ours = ours;
    c.theirs = theirs;
    result.conflicts.push_back(std::move(c));
    return result;
}

} // namespace

bool is_id_keyed_array(const JsonValue& array) noexcept
{
    if (array.type != JsonValue::Type::array || array.elements.empty())
        return false;
    std::unordered_set<std::string> seen;
    for (const JsonValue& el : array.elements)
    {
        if (el.type != JsonValue::Type::object)
            return false;
        const JsonValue* id = member(el, "id");
        if (id == nullptr || id->type != JsonValue::Type::string || id->string_value.empty())
            return false;
        // A DUPLICATE intra-file id makes id-identity merging unsafe: merge_id_array's per-id match
        // assumes uniqueness per side and would silently drop/misplace a colliding element (the very
        // corruption `context re-key` exists to remedy). Treat such an array as opaque so merge_value
        // surfaces a whole-array field conflict (ours preserved) instead of silently unifying it.
        if (!seen.insert(id->string_value).second)
            return false;
    }
    return true;
}

MergeResult merge_documents(const JsonValue& base, const JsonValue& ours, const JsonValue& theirs,
                            const MergeOptions& options)
{
    // Whole-file classes are decided before any structural merge — the driver must refuse honestly
    // at file granularity rather than blend data it cannot field-merge.

    // (a) Newer-stamped payloads (L-37 / R-FILE-012(a)): an input stamped beyond the installed
    //     schemas is a whole-file ours/theirs conflict, NEVER a parse error.
    if (exceeds_floor(ours, options.floor) || exceeds_floor(theirs, options.floor))
        return whole_file(ConflictClass::newer_stamped, base, ours, theirs);

    // (d) Meta-GUID (L-36 / R-FILE-012(d)): both sides minted a DIFFERENT GUID for the same asset
    //     meta — whole-asset ours/theirs, GUID identity is never field-blended.
    if (options.detect_meta_guid)
    {
        const JsonValue* og = member(ours, "guid");
        const JsonValue* tg = member(theirs, "guid");
        if (og != nullptr && tg != nullptr && og->type == JsonValue::Type::string &&
            tg->type == JsonValue::Type::string && og->string_value != tg->string_value)
        {
            // Fire ONLY when BOTH sides minted a guid different from base (a genuine both-sides mint).
            // A one-sided guid edit (base == ours, or base == theirs) is not a conflict — it
            // auto-merges to the changed side like any other field; forcing it whole-file would
            // spuriously conflict a clean, one-sided re-guid.
            const JsonValue* bg = member(base, "guid");
            const bool base_has_guid = bg != nullptr && bg->type == JsonValue::Type::string;
            const bool both_diverged =
                !base_has_guid || (bg->string_value != og->string_value &&
                                   bg->string_value != tg->string_value);
            if (both_diverged)
                return whole_file(ConflictClass::meta_guid, base, ours, theirs);
        }
    }

    MergeResult result;
    result.merged = merge_value(base, ours, theirs, "", result.conflicts);
    result.clean = result.conflicts.empty();
    result.whole_file = false;
    return result;
}

} // namespace context::editor::merge
