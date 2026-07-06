// Runtime registration of declarative component types into data-driven archetype storage (R-LANG-010).
//
// A compiled ComponentTypeSchema (component_type.h) is registered here: the registry allocates a
// runtime kernel::ComponentId, derives the type-erased kernel::ComponentOps for the packed record
// (a trivially-relocatable POD blob — memcpy relocate, no destructor), and stores the pair under the
// type's STABLE name. Adding a component of a declared type to an entity then goes straight through
// the kernel World's raw (non-template) storage API (world.h) — so a project or npm package defines
// a new component type at LOAD time and it lands in the SAME archetype/SoA storage the built-in
// compile-time components use, with NO native engine rebuild (L-60).
//
// Payloads round-trip through the M2 canonical serializer: encode_payload writes a JSON object's
// fields into a record blob at the derived offsets; read_payload reads a record blob back into a
// canonical JSON object. The numeric domain is the schema's per-field storage width.

#pragma once

#include "context/editor/component/component_type.h"
#include "context/editor/serializer/json_tree.h"
#include "context/kernel/component.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::component
{

// One registered declarative component type: the compiled schema, the runtime ComponentId the
// registry allocated for it, and the type-erased ops the World stores its column with.
struct RegisteredComponentType
{
    ComponentTypeSchema schema;
    kernel::ComponentId component_id = 0;
    kernel::ComponentOps ops;
};

// A set of declarative component types registered for a World (or a describe surface). Names are the
// stable cross-process identity (unlike the runtime ComponentId, assigned in registration order);
// re-registering the same id REPLACES the entry (idempotent), the same contract as SchemaSet.
class ComponentTypeRegistry
{
public:
    // Register `schema` (or replace the entry with the same id) and return the registration. The
    // allocated ComponentId is stable for the life of this registry. Ops are the POD-blob ops for
    // the derived record layout (trivially relocatable; kernel::pod_ops). The returned reference (and
    // any earlier by_id/register_type reference) stays valid across later register_type calls — the
    // registry stores entries in a node-stable container on purpose, so a caller can hold a handle to
    // one registered type while registering more.
    const RegisteredComponentType& register_type(ComponentTypeSchema schema);

    [[nodiscard]] const RegisteredComponentType* by_id(std::string_view id) const noexcept;
    [[nodiscard]] const RegisteredComponentType*
    by_component_id(kernel::ComponentId id) const noexcept;
    [[nodiscard]] const std::deque<RegisteredComponentType>& all() const noexcept { return types_; }

    // Add a default (all-zero) instance of `type` onto entity `e` in `w`, returning the record
    // storage to write into (nullptr if `e` is dead). Data-driven: no compile-time knowledge of the
    // type — the record is a `type.schema.size`-byte blob in the World's archetype column.
    [[nodiscard]] void* add_default(kernel::World& w, kernel::Entity e,
                                    const RegisteredComponentType& type) const;

private:
    // A node-stable container (references survive push_back), so register_type's returned reference
    // never dangles when more types register afterwards.
    std::deque<RegisteredComponentType> types_;
};

// --- canonical payload round-trip (M2 serializer) ------------------------------------------------

// Write the fields of the JSON object `payload` into the record blob `dst` (>= type.schema.size
// bytes) at the derived offsets. Missing fields keep `dst`'s current bytes (so a partial payload
// patches an existing record); unknown members and a non-object payload are reported. A field value
// out of its storage width's range, or of the wrong JSON kind, is reported. Returns true iff no
// problems were appended.
[[nodiscard]] bool encode_payload(const RegisteredComponentType& type,
                                  const serializer::JsonValue& payload, void* dst,
                                  std::vector<std::string>& problems);

// Read the record blob `src` (>= type.schema.size bytes) into a canonical JSON object: one member
// per field, in declared order, each a JSON number/integer of the field's storage width (scalar
// fields as a single value, lanes > 1 as an array). The inverse of encode_payload for in-range data.
[[nodiscard]] serializer::JsonValue read_payload(const RegisteredComponentType& type,
                                                 const void* src);

// read_payload serialized to canonical JSON (the published payload form). Empty string on the
// unreachable non-finite case (component payloads carry only finite numbers).
[[nodiscard]] std::string serialize_payload(const RegisteredComponentType& type, const void* src);

// The engine-owned declarative component types. EMPTY in v1: the engine ships no built-in declarative
// component types — they are project/package-defined at load time, which is R-LANG-010's whole point
// (the built-in compile-time sim components live in src/runtime/session/). The set exists so the
// contract registry enumerates component types LIVE (describe's componentTypes section) exactly as it
// enumerates engine_schemas() for file kinds; package/project types join through register_type().
[[nodiscard]] const ComponentTypeRegistry& engine_component_types();

} // namespace context::editor::component
