#include "canonical.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

namespace ctx {

void ecmaNumber(double v, std::string& out) {
    if (v == 0.0) {  // covers -0: serialized as "0" (R-FILE-001)
        out.push_back('0');
        return;
    }
    bool neg = v < 0.0;
    if (neg) v = -v;

    // Shortest round-trip digits via to_chars in scientific form: d[.ddddd]e±dd
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    const char* p = buf;
    const char* end = res.ptr;

    char digits[32];
    int k = 0;  // digit count
    digits[k++] = *p++;  // leading digit
    if (p < end && *p == '.') {
        ++p;
        while (p < end && *p != 'e') digits[k++] = *p++;
    }
    // exponent
    int e10 = 0;
    if (p < end && *p == 'e') {
        ++p;
        bool eneg = (*p == '-');
        if (*p == '+' || *p == '-') ++p;
        while (p < end) e10 = e10 * 10 + (*p++ - '0');
        if (eneg) e10 = -e10;
    }
    // Strip trailing zero digits (to_chars shortest form should not produce any,
    // but be safe); value = 0.digits * 10^n with n = e10 + 1.
    while (k > 1 && digits[k - 1] == '0') --k;
    int n = e10 + 1;

    if (neg) out.push_back('-');
    if (k <= n && n <= 21) {
        out.append(digits, static_cast<size_t>(k));
        out.append(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, static_cast<size_t>(n));
        out.push_back('.');
        out.append(digits + n, static_cast<size_t>(k - n));
    } else if (-6 < n && n <= 0) {
        out.append("0.");
        out.append(static_cast<size_t>(-n), '0');
        out.append(digits, static_cast<size_t>(k));
    } else {
        int e = n - 1;
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out.append(digits + 1, static_cast<size_t>(k - 1));
        }
        out.push_back('e');
        out.push_back(e >= 0 ? '+' : '-');
        char ebuf[16];
        auto er = std::to_chars(ebuf, ebuf + sizeof(ebuf), e >= 0 ? e : -e);
        out.append(ebuf, static_cast<size_t>(er.ptr - ebuf));
    }
}

namespace {

void writeString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    char ubuf[8];
                    std::snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
                    out.append(ubuf);
                } else {
                    out.push_back(static_cast<char>(c));  // raw UTF-8 pass-through
                }
        }
    }
    out.push_back('"');
}

void writeValue(const JsonValue& v, int indent, std::string& out) {
    switch (v.type) {
        case JsonValue::Type::Null: out.append("null"); return;
        case JsonValue::Type::Bool: out.append(v.boolean ? "true" : "false"); return;
        case JsonValue::Type::Int: {
            char b[24];
            auto r = std::to_chars(b, b + sizeof(b), v.i64);
            out.append(b, static_cast<size_t>(r.ptr - b));
            return;
        }
        case JsonValue::Type::Uint: {
            char b[24];
            auto r = std::to_chars(b, b + sizeof(b), v.u64);
            out.append(b, static_cast<size_t>(r.ptr - b));
            return;
        }
        case JsonValue::Type::Double: ecmaNumber(v.dbl, out); return;
        case JsonValue::Type::String: writeString(v.str, out); return;
        case JsonValue::Type::Array: {
            if (v.arr.empty()) {
                out.append("[]");
                return;
            }
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out.push_back('\n');
                out.append(static_cast<size_t>(indent + 1) * 2, ' ');
                writeValue(v.arr[i], indent + 1, out);
                if (i + 1 < v.arr.size()) out.push_back(',');
            }
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * 2, ' ');
            out.push_back(']');
            return;
        }
        case JsonValue::Type::Object: {
            if (v.obj.empty()) {
                out.append("{}");
                return;
            }
            // Sort member indices by key (UTF-8 byte order) — stable key order.
            std::vector<uint32_t> idx(v.obj.size());
            std::iota(idx.begin(), idx.end(), 0u);
            std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
                return v.obj[a].key < v.obj[b].key;
            });
            out.push_back('{');
            for (size_t i = 0; i < idx.size(); ++i) {
                const JsonMember& m = v.obj[idx[i]];
                out.push_back('\n');
                out.append(static_cast<size_t>(indent + 1) * 2, ' ');
                writeString(m.key, out);
                out.append(": ");
                writeValue(m.value, indent + 1, out);
                if (i + 1 < idx.size()) out.push_back(',');
            }
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * 2, ' ');
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

void canonicalWrite(const JsonValue& v, std::string& out) {
    writeValue(v, 0, out);
}

}  // namespace ctx
