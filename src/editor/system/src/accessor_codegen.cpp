// Derived TS accessor codegen — see accessor_codegen.h. Deterministic string emission: one DataView
// getter/setter per scalar field, addressed by the field's derived byte offset, over a little-endian
// record layout (matching the host's native scalar bytes on every LE CI target).

#include "context/editor/system/accessor_codegen.h"

#include <cctype>
#include <string>
#include <string_view>

namespace context::editor::system
{
namespace
{

using component::ComponentField;
using component::ComponentTypeSchema;
using component::ScalarKind;

// The DataView accessor spelling for one scalar base: the method suffix (getFloat32/setFloat32/...),
// the TS element type, and whether the accessor takes a little-endian flag (all multi-byte scalars do;
// the two 1-byte families do not — DataView.getUint8/getInt8 have no endianness argument).
struct DvSpec
{
    std::string_view suffix;      // e.g. "Float32" -> getFloat32 / setFloat32
    std::string_view ts_type;     // "number" or "bigint"
    bool little_endian_arg;       // append ", true" to the DataView call
};

[[nodiscard]] DvSpec dv_spec(ScalarKind kind) noexcept
{
    switch (kind)
    {
    case ScalarKind::f32:
        return {"Float32", "number", true};
    case ScalarKind::f64:
        return {"Float64", "number", true};
    case ScalarKind::i8:
        return {"Int8", "number", false};
    case ScalarKind::i16:
        return {"Int16", "number", true};
    case ScalarKind::i32:
        return {"Int32", "number", true};
    case ScalarKind::i64:
        return {"BigInt64", "bigint", true};
    case ScalarKind::u8:
        return {"Uint8", "number", false};
    case ScalarKind::u16:
        return {"Uint16", "number", true};
    case ScalarKind::u32:
        return {"Uint32", "number", true};
    case ScalarKind::u64:
        return {"BigUint64", "bigint", true};
    }
    return {"Uint8", "number", false}; // unreachable: the switch is exhaustive over ScalarKind
}

// PascalCase a field name ("max_hp" -> "MaxHp"): split on '_', capitalize each non-empty run's first
// letter. Field names are validated `[a-z][a-z0-9_]*` by the component compiler, so this is total.
[[nodiscard]] std::string pascal_case(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    bool at_word_start = true;
    for (const char c : name)
    {
        if (c == '_')
        {
            at_word_start = true;
            continue;
        }
        if (at_word_start)
        {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            at_word_start = false;
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

// The little-endian flag argument appended inside a DataView call, or "" for a 1-byte scalar.
[[nodiscard]] std::string le_arg(const DvSpec& spec)
{
    return spec.little_endian_arg ? ", true" : "";
}

// The byte-offset expression for (row, lane) of a field at record offset `field_offset`, scalar width
// `width`: "row * <cls>.stride + <field_offset> + lane * <width>". The lane term is dropped for a
// scalar field (lanes == 1) so its accessor takes no `lane` parameter.
[[nodiscard]] std::string offset_expr(std::string_view cls, std::size_t field_offset,
                                      std::size_t width, bool with_lane)
{
    std::string e = "row * " + std::string(cls) + ".stride + " + std::to_string(field_offset);
    if (with_lane)
    {
        e += " + lane * " + std::to_string(width);
    }
    return e;
}

} // namespace

std::string accessor_class_name(std::string_view id)
{
    std::string out;
    out.reserve(id.size() + 1);
    for (const char c : id)
    {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(uc) != 0 || c == '_') ? c : '_');
    }
    if (out.empty() || (std::isdigit(static_cast<unsigned char>(out.front())) != 0))
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string generate_component_accessor_ts(const ComponentTypeSchema& type)
{
    const std::string cls = accessor_class_name(type.id);

    std::string ts;
    ts += "// Auto-generated zero-copy accessor for component \"" + type.id + "\" (schema version " +
          std::to_string(type.version) + ", layout hash 0x";
    // Lowercase 16-digit hex of the layout hash — stable identity of the record layout this accessor
    // is bound to (a shape change re-derives the accessor).
    {
        static const char* kHex = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4)
        {
            ts.push_back(kHex[(type.layout_hash >> shift) & 0xFULL]);
        }
    }
    ts += "). DO NOT EDIT — regenerate from the declarative definition.\n";
    ts += "export class " + cls + "\n{\n";
    ts += "    readonly dv: DataView;\n";
    ts += "    static readonly stride: number = " + std::to_string(type.size) + ";\n";
    ts += "    constructor(view: Uint8Array)\n    {\n";
    ts += "        this.dv = new DataView(view.buffer, view.byteOffset, view.byteLength);\n";
    ts += "    }\n";
    ts += "    get count(): number\n    {\n";
    ts += "        return (this.dv.byteLength / " + cls + ".stride) | 0;\n";
    ts += "    }\n";

    for (const ComponentField& f : type.fields)
    {
        const DvSpec spec = dv_spec(f.storage.base);
        const std::size_t width = component::scalar_byte_width(f.storage.base);
        const bool multi = f.storage.lanes > 1;
        const std::string method = pascal_case(f.name);
        const std::string t = std::string(spec.ts_type);
        const std::string get_call = "this.dv.get" + std::string(spec.suffix);
        const std::string set_call = "this.dv.set" + std::string(spec.suffix);

        ts += "    // field \"" + f.name + "\": " +
              std::string(component::scalar_kind_id(f.storage.base));
        if (multi)
        {
            ts += " x" + std::to_string(f.storage.lanes);
        }
        ts += " @ offset " + std::to_string(f.offset) + "\n";

        const std::string params = multi ? "row: number, lane: number" : "row: number";
        const std::string off = offset_expr(cls, f.offset, width, multi);

        ts += "    get" + method + "(" + params + "): " + t + "\n    {\n";
        ts += "        return " + get_call + "(" + off + le_arg(spec) + ");\n";
        ts += "    }\n";
        ts += "    set" + method + "(" + params + ", v: " + t + "): void\n    {\n";
        ts += "        " + set_call + "(" + off + ", v" + le_arg(spec) + ");\n";
        ts += "    }\n";
    }

    ts += "}\n";
    return ts;
}

} // namespace context::editor::system
