// Runtime registration of declarative component types into data-driven archetype storage (R-LANG-010)
// + the canonical-JSON payload round-trip. See component_registry.h for the mechanism overview.

#include "context/editor/component/component_registry.h"

#include "context/editor/serializer/canonical.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace context::editor::component
{
namespace
{

using serializer::JsonValue;

// Extract a numeric value from a JSON scalar as a double (for float fields). Booleans/strings/etc.
// are not numbers. Returns false when `v` is not a numeric JSON value.
[[nodiscard]] bool as_double(const JsonValue& v, double& out) noexcept
{
    switch (v.type)
    {
    case JsonValue::Type::integer:
        out = static_cast<double>(v.int_value);
        return true;
    case JsonValue::Type::unsigned_integer:
        out = static_cast<double>(v.uint_value);
        return true;
    case JsonValue::Type::number:
        out = v.number_value;
        return true;
    default:
        return false;
    }
}

// Extract an integer value from a JSON scalar (for integer fields — a fractional `number` is NOT a
// valid integer payload). `is_negative` reports the sign so unsigned-range checks can reject it.
[[nodiscard]] bool as_int(const JsonValue& v, std::int64_t& sval, std::uint64_t& uval,
                          bool& is_unsigned) noexcept
{
    switch (v.type)
    {
    case JsonValue::Type::integer:
        sval = v.int_value;
        is_unsigned = false;
        return true;
    case JsonValue::Type::unsigned_integer:
        uval = v.uint_value;
        is_unsigned = true;
        return true;
    default:
        return false;
    }
}

// Write one scalar of `kind` from JSON value `v` into `dst`. On a type/range error appends a message
// to `err` (prefixed with `where`) and returns false.
[[nodiscard]] bool write_scalar(ScalarKind kind, const JsonValue& v, void* dst,
                                const std::string& where, std::vector<std::string>& err)
{
    auto range_fail = [&](std::string_view what)
    {
        err.push_back(where + ": value out of range for " + std::string(what));
        return false;
    };

    if (kind == ScalarKind::f32 || kind == ScalarKind::f64)
    {
        double d = 0.0;
        if (!as_double(v, d))
        {
            err.push_back(where + ": expected a number");
            return false;
        }
        if (kind == ScalarKind::f32)
        {
            const auto f = static_cast<float>(d);
            std::memcpy(dst, &f, sizeof(f));
        }
        else
        {
            std::memcpy(dst, &d, sizeof(d));
        }
        return true;
    }

    std::int64_t sval = 0;
    std::uint64_t uval = 0;
    bool is_unsigned = false;
    if (!as_int(v, sval, uval, is_unsigned))
    {
        err.push_back(where + ": expected an integer");
        return false;
    }

    // Normalize to a signed 128-ish view via two paths: a signed source keeps its sign; an unsigned
    // source is non-negative. Each width checks its own bounds.
    auto store = [&](auto typed) { std::memcpy(dst, &typed, sizeof(typed)); };

    switch (kind)
    {
    case ScalarKind::i8:
        if (is_unsigned ? uval > 127U : (sval < -128 || sval > 127))
            return range_fail("i8");
        store(static_cast<std::int8_t>(is_unsigned ? static_cast<std::int64_t>(uval) : sval));
        return true;
    case ScalarKind::i16:
        if (is_unsigned ? uval > 32767U : (sval < -32768 || sval > 32767))
            return range_fail("i16");
        store(static_cast<std::int16_t>(is_unsigned ? static_cast<std::int64_t>(uval) : sval));
        return true;
    case ScalarKind::i32:
        if (is_unsigned ? uval > 2147483647U : (sval < -2147483648LL || sval > 2147483647LL))
            return range_fail("i32");
        store(static_cast<std::int32_t>(is_unsigned ? static_cast<std::int64_t>(uval) : sval));
        return true;
    case ScalarKind::i64:
        if (is_unsigned && uval > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return range_fail("i64");
        store(is_unsigned ? static_cast<std::int64_t>(uval) : sval);
        return true;
    case ScalarKind::u8:
        if (is_unsigned ? uval > 255U : (sval < 0 || sval > 255))
            return range_fail("u8");
        store(static_cast<std::uint8_t>(is_unsigned ? uval : static_cast<std::uint64_t>(sval)));
        return true;
    case ScalarKind::u16:
        if (is_unsigned ? uval > 65535U : (sval < 0 || sval > 65535))
            return range_fail("u16");
        store(static_cast<std::uint16_t>(is_unsigned ? uval : static_cast<std::uint64_t>(sval)));
        return true;
    case ScalarKind::u32:
        if (is_unsigned ? uval > 4294967295ULL : (sval < 0 || sval > 4294967295LL))
            return range_fail("u32");
        store(static_cast<std::uint32_t>(is_unsigned ? uval : static_cast<std::uint64_t>(sval)));
        return true;
    case ScalarKind::u64:
        if (!is_unsigned && sval < 0)
            return range_fail("u64");
        store(is_unsigned ? uval : static_cast<std::uint64_t>(sval));
        return true;
    default:
        break;
    }
    return false; // unreachable: the switch is exhaustive over ScalarKind
}

// Read one scalar of `kind` from `src` into a canonical JSON number/integer value.
[[nodiscard]] JsonValue read_scalar(ScalarKind kind, const void* src)
{
    JsonValue v;
    auto load = [&](auto& typed) { std::memcpy(&typed, src, sizeof(typed)); };
    auto put_signed = [&](std::int64_t n)
    {
        v.type = JsonValue::Type::integer;
        v.int_value = n;
    };
    auto put_unsigned = [&](std::uint64_t n)
    {
        if (n <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            v.type = JsonValue::Type::integer;
            v.int_value = static_cast<std::int64_t>(n);
        }
        else
        {
            v.type = JsonValue::Type::unsigned_integer;
            v.uint_value = n;
        }
    };

    switch (kind)
    {
    case ScalarKind::f32:
    {
        float f = 0.0F;
        load(f);
        v.type = JsonValue::Type::number;
        v.number_value = static_cast<double>(f);
        break;
    }
    case ScalarKind::f64:
    {
        double d = 0.0;
        load(d);
        v.type = JsonValue::Type::number;
        v.number_value = d;
        break;
    }
    case ScalarKind::i8:
    {
        std::int8_t n = 0;
        load(n);
        put_signed(n);
        break;
    }
    case ScalarKind::i16:
    {
        std::int16_t n = 0;
        load(n);
        put_signed(n);
        break;
    }
    case ScalarKind::i32:
    {
        std::int32_t n = 0;
        load(n);
        put_signed(n);
        break;
    }
    case ScalarKind::i64:
    {
        std::int64_t n = 0;
        load(n);
        put_signed(n);
        break;
    }
    case ScalarKind::u8:
    {
        std::uint8_t n = 0;
        load(n);
        put_unsigned(n);
        break;
    }
    case ScalarKind::u16:
    {
        std::uint16_t n = 0;
        load(n);
        put_unsigned(n);
        break;
    }
    case ScalarKind::u32:
    {
        std::uint32_t n = 0;
        load(n);
        put_unsigned(n);
        break;
    }
    case ScalarKind::u64:
    {
        std::uint64_t n = 0;
        load(n);
        put_unsigned(n);
        break;
    }
    }
    return v;
}

const JsonValue* find_member(const JsonValue& object, std::string_view key) noexcept
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

} // namespace

const RegisteredComponentType& ComponentTypeRegistry::register_type(ComponentTypeSchema schema)
{
    for (RegisteredComponentType& existing : types_)
        if (existing.schema.id == schema.id)
        {
            // Replace in place, keeping the already-allocated ComponentId stable for this registry.
            existing.schema = std::move(schema);
            existing.ops = kernel::pod_ops(existing.schema.size, existing.schema.align);
            return existing;
        }

    RegisteredComponentType reg;
    reg.component_id = kernel::detail::next_component_id();
    reg.ops = kernel::pod_ops(schema.size, schema.align);
    reg.schema = std::move(schema);
    types_.push_back(std::move(reg));
    return types_.back();
}

const RegisteredComponentType* ComponentTypeRegistry::by_id(std::string_view id) const noexcept
{
    for (const RegisteredComponentType& t : types_)
        if (t.schema.id == id)
            return &t;
    return nullptr;
}

const RegisteredComponentType*
ComponentTypeRegistry::by_component_id(kernel::ComponentId id) const noexcept
{
    for (const RegisteredComponentType& t : types_)
        if (t.component_id == id)
            return &t;
    return nullptr;
}

void* ComponentTypeRegistry::add_default(kernel::World& w, kernel::Entity e,
                                         const RegisteredComponentType& type) const
{
    // A zero-initialized record straight into the World's archetype/SoA storage (data-driven: the
    // World knows only the ComponentId + POD ops, never the C++ type).
    return w.add_raw(e, type.component_id, type.ops, nullptr);
}

bool encode_payload(const RegisteredComponentType& type, const serializer::JsonValue& payload,
                    void* dst, std::vector<std::string>& problems)
{
    if (payload.type != JsonValue::Type::object)
    {
        problems.emplace_back("payload must be a JSON object");
        return false;
    }

    const std::size_t start = problems.size();
    auto* bytes = static_cast<std::byte*>(dst);

    // Reject members that are not declared fields (a typo must not silently no-op).
    for (const serializer::JsonMember& m : payload.members)
        if (type.schema.field(m.key) == nullptr)
            problems.push_back("/" + m.key + ": no such field on component \"" + type.schema.id +
                               "\"");

    for (const ComponentField& f : type.schema.fields)
    {
        const JsonValue* member = find_member(payload, f.name);
        if (member == nullptr)
            continue; // absent field keeps dst's current bytes (partial-patch semantics)

        const std::size_t width = scalar_byte_width(f.storage.base);
        const std::string ptr = "/" + f.name;
        if (f.storage.lanes == 1)
        {
            (void)write_scalar(f.storage.base, *member, bytes + f.offset, ptr, problems);
        }
        else if (member->type != JsonValue::Type::array ||
                 member->elements.size() != f.storage.lanes)
        {
            problems.push_back(ptr + ": expected an array of " + std::to_string(f.storage.lanes) +
                               " numbers");
        }
        else
        {
            for (unsigned lane = 0; lane < f.storage.lanes; ++lane)
                (void)write_scalar(f.storage.base, member->elements[lane],
                                   bytes + f.offset + lane * width,
                                   ptr + "/" + std::to_string(lane), problems);
        }
    }

    return problems.size() == start;
}

serializer::JsonValue read_payload(const RegisteredComponentType& type, const void* src)
{
    const auto* bytes = static_cast<const std::byte*>(src);
    JsonValue obj;
    obj.type = JsonValue::Type::object;
    for (const ComponentField& f : type.schema.fields)
    {
        const std::size_t width = scalar_byte_width(f.storage.base);
        JsonValue value;
        if (f.storage.lanes == 1)
        {
            value = read_scalar(f.storage.base, bytes + f.offset);
        }
        else
        {
            value.type = JsonValue::Type::array;
            for (unsigned lane = 0; lane < f.storage.lanes; ++lane)
                value.elements.push_back(read_scalar(f.storage.base, bytes + f.offset + lane * width));
        }
        obj.members.push_back(serializer::JsonMember{f.name, std::move(value)});
    }
    return obj;
}

std::string serialize_payload(const RegisteredComponentType& type, const void* src)
{
    const JsonValue payload = read_payload(type, src);
    std::string out;
    if (!serializer::serialize_canonical(payload, out))
        out.clear(); // unreachable: a component payload holds only finite numbers
    return out;
}

const ComponentTypeRegistry& engine_component_types()
{
    static const ComponentTypeRegistry registry; // empty in v1 (see component_registry.h)
    return registry;
}

} // namespace context::editor::component
