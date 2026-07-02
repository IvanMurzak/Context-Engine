#include "merge.h"

#include <unordered_map>

namespace ctx {

namespace {

bool equalsOpt(const JsonValue* a, const JsonValue* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return deepEquals(*a, *b);
}

// An id-keyed collection (L-33): non-empty array whose every element is an object
// carrying a string "id" member.
bool isIdKeyed(const JsonValue& v) {
    if (v.type != JsonValue::Type::Array || v.arr.empty()) return false;
    for (const auto& e : v.arr) {
        if (e.type != JsonValue::Type::Object) return false;
        const JsonValue* id = e.find("id");
        if (id == nullptr || id->type != JsonValue::Type::String) return false;
    }
    return true;
}

struct Ctx {
    std::string path;
    std::vector<Conflict>* conflicts;

    void conflictHere() { conflicts->push_back(Conflict{path}); }
};

// Returns true if the merged node is present; if so, `out` holds it.
bool mergeNode(const JsonValue* b, const JsonValue* o, const JsonValue* t, Ctx& ctx,
               JsonValue& out);

void mergeObjects(const JsonValue* b, const JsonValue& o, const JsonValue& t, Ctx& ctx,
                  JsonValue& out) {
    out.type = JsonValue::Type::Object;
    out.obj.clear();
    const size_t pathLen = ctx.path.size();
    // Union of keys: ours order first, then theirs-only keys in theirs order.
    for (const auto& [key, oval] : o.obj) {
        const JsonValue* bv = b ? b->find(key) : nullptr;
        const JsonValue* tv = t.find(key);
        ctx.path.push_back('/');
        ctx.path.append(key);
        JsonValue merged;
        if (mergeNode(bv, &oval, tv, ctx, merged))
            out.obj.emplace_back(key, std::move(merged));
        ctx.path.resize(pathLen);
    }
    for (const auto& [key, tval] : t.obj) {
        if (o.find(key) != nullptr) continue;  // handled above
        const JsonValue* bv = b ? b->find(key) : nullptr;
        ctx.path.push_back('/');
        ctx.path.append(key);
        JsonValue merged;
        if (mergeNode(bv, nullptr, &tval, ctx, merged))
            out.obj.emplace_back(key, std::move(merged));
        ctx.path.resize(pathLen);
    }
}

void mergeIdKeyed(const JsonValue* b, const JsonValue& o, const JsonValue& t, Ctx& ctx,
                  JsonValue& out) {
    out.type = JsonValue::Type::Array;
    out.arr.clear();
    auto indexOf = [](const JsonValue* v) {
        std::unordered_map<std::string, const JsonValue*> m;
        if (v != nullptr && v->type == JsonValue::Type::Array) {
            for (const auto& e : v->arr) {
                const JsonValue* id = e.find("id");
                if (id != nullptr && id->type == JsonValue::Type::String)
                    m.emplace(id->str, &e);
            }
        }
        return m;
    };
    const auto bIdx = indexOf(b);
    const auto oIdx = indexOf(&o);
    const auto tIdx = indexOf(&t);

    const size_t pathLen = ctx.path.size();
    auto emit = [&](const std::string& id) {
        auto bit = bIdx.find(id);
        auto oit = oIdx.find(id);
        auto tit = tIdx.find(id);
        const JsonValue* bv = bit == bIdx.end() ? nullptr : bit->second;
        const JsonValue* ov = oit == oIdx.end() ? nullptr : oit->second;
        const JsonValue* tv = tit == tIdx.end() ? nullptr : tit->second;
        ctx.path.push_back('/');
        ctx.path.append(id);
        if (bv == nullptr && ov != nullptr && tv != nullptr) {
            // Same id ADDED on both sides relative to base: field-merge only if
            // identical; otherwise a STRUCTURAL conflict, never silent unification
            // (R-FILE-012(b)).
            if (deepEquals(*ov, *tv)) {
                out.arr.push_back(*ov);
            } else {
                ctx.conflictHere();
                out.arr.push_back(*ov);  // take-ours placeholder in the merged tree
            }
        } else {
            JsonValue merged;
            if (mergeNode(bv, ov, tv, ctx, merged)) out.arr.push_back(std::move(merged));
        }
        ctx.path.resize(pathLen);
    };

    // Stable order: base order, then ours-added, then theirs-added.
    if (b != nullptr && b->type == JsonValue::Type::Array) {
        for (const auto& e : b->arr) {
            const JsonValue* id = e.find("id");
            if (id != nullptr) emit(id->str);
        }
    }
    for (const auto& e : o.arr) {
        const JsonValue* id = e.find("id");
        if (id != nullptr && bIdx.find(id->str) == bIdx.end()) emit(id->str);
    }
    for (const auto& e : t.arr) {
        const JsonValue* id = e.find("id");
        if (id != nullptr && bIdx.find(id->str) == bIdx.end() &&
            oIdx.find(id->str) == oIdx.end())
            emit(id->str);
    }
}

bool mergeNode(const JsonValue* b, const JsonValue* o, const JsonValue* t, Ctx& ctx,
               JsonValue& out) {
    // Identical on both sides (including both deleted): take it.
    if (equalsOpt(o, t)) {
        if (o == nullptr) return false;
        out = *o;
        return true;
    }
    // Only one side changed relative to base: the changed side wins.
    if (equalsOpt(b, o)) {
        if (t == nullptr) return false;
        out = *t;
        return true;
    }
    if (equalsOpt(b, t)) {
        if (o == nullptr) return false;
        out = *o;
        return true;
    }
    // Both sides changed, differently.
    if (o != nullptr && t != nullptr) {
        if (o->type == JsonValue::Type::Object && t->type == JsonValue::Type::Object) {
            const JsonValue* bObj =
                (b != nullptr && b->type == JsonValue::Type::Object) ? b : nullptr;
            mergeObjects(bObj, *o, *t, ctx, out);
            return true;
        }
        if (isIdKeyed(*o) && isIdKeyed(*t)) {
            mergeIdKeyed(b, *o, *t, ctx, out);
            return true;
        }
    }
    // Scalar / mixed / delete-vs-modify: field-path conflict. The merged tree keeps
    // the "ours" state; the conflict envelope carries the path (R-FILE-012 — no text
    // markers, no silent last-writer-wins: the conflict IS reported).
    ctx.conflictHere();
    if (o == nullptr) return false;
    out = *o;
    return true;
}

}  // namespace

MergeResult merge3(const JsonValue* base, const JsonValue* ours, const JsonValue* theirs) {
    MergeResult result;
    Ctx ctx;
    ctx.conflicts = &result.conflicts;
    if (!mergeNode(base, ours, theirs, ctx, result.merged))
        result.merged = JsonValue{};  // whole-document deletion
    return result;
}

}  // namespace ctx
