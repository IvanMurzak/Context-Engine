// parse-bench spike — mutable JSON tree (throwaway M0 measurement code).
//
// simdjson's DOM is read-only; canonicalization and the three-way structural merge
// need a mutable, ordered tree. This is the minimal such tree: object members keep
// authored order (the canonical writer sorts keys at serialization time), arrays keep
// authored order (stable array ordering, R-FILE-001).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ctx {

struct JsonValue {
    enum class Type : uint8_t { Null, Bool, Int, Uint, Double, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    double dbl = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    static JsonValue makeString(std::string s) {
        JsonValue v;
        v.type = Type::String;
        v.str = std::move(s);
        return v;
    }
    static JsonValue makeDouble(double d) {
        JsonValue v;
        v.type = Type::Double;
        v.dbl = d;
        return v;
    }

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    JsonValue* find(const std::string& key) {
        if (type != Type::Object) return nullptr;
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
};

// Deep structural equality (numbers compare by value within their lexical class;
// the corpus contains no NaN — R-FILE-001 bans it).
inline bool deepEquals(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonValue::Type::Null: return true;
        case JsonValue::Type::Bool: return a.boolean == b.boolean;
        case JsonValue::Type::Int: return a.i64 == b.i64;
        case JsonValue::Type::Uint: return a.u64 == b.u64;
        case JsonValue::Type::Double: return a.dbl == b.dbl;
        case JsonValue::Type::String: return a.str == b.str;
        case JsonValue::Type::Array: {
            if (a.arr.size() != b.arr.size()) return false;
            for (size_t i = 0; i < a.arr.size(); ++i)
                if (!deepEquals(a.arr[i], b.arr[i])) return false;
            return true;
        }
        case JsonValue::Type::Object: {
            if (a.obj.size() != b.obj.size()) return false;
            // Canonical files have deterministically ordered keys, so positional
            // comparison is valid for corpus data; fall back to lookup if order drifts.
            for (size_t i = 0; i < a.obj.size(); ++i) {
                if (a.obj[i].first == b.obj[i].first) {
                    if (!deepEquals(a.obj[i].second, b.obj[i].second)) return false;
                } else {
                    const JsonValue* other = b.find(a.obj[i].first);
                    if (!other || !deepEquals(a.obj[i].second, *other)) return false;
                }
            }
            return true;
        }
    }
    return false;
}

}  // namespace ctx
