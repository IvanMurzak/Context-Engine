// glTF importer — GLB container unwrap + glTF-2.0 JSON parse (via the engine's canonical parser)
// into a canonical mesh descriptor with the R-REND-006 UV2 (lightmap) channel reserved.

#include "context/editor/import/importers/gltf_importer.h"

#include "context/editor/import/platform_profile.h"
#include "context/editor/serializer/canonical.h"

#include "../detail/json_detail.h"

#include <cstdint>
#include <string>
#include <utility>

namespace context::editor::import
{
namespace
{
using detail::append;
using detail::as_int64;
using detail::jarray;
using detail::jbool;
using detail::jint;
using detail::jobject;
using detail::jstr;
using detail::juint;
using detail::member;
using detail::put;
using serializer::JsonValue;

std::uint32_t read_u32le(std::string_view b, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

void fail(std::vector<ImportDiagnostic>& diagnostics, std::string code, std::string message)
{
    diagnostics.push_back({std::move(code), std::move(message)});
}
} // namespace

bool parse_gltf(std::string_view bytes, GltfInfo& info, std::vector<ImportDiagnostic>& diagnostics)
{
    std::string_view json_bytes;
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "glTF")
    {
        // GLB binary container: 12-byte header (magic, version, length) + JSON chunk (+ BIN chunk).
        info.is_glb = true;
        const std::uint32_t container_version = read_u32le(bytes, 4);
        if (container_version != 2)
        {
            fail(diagnostics, "import.unsupported_format", "unsupported GLB container version");
            return false;
        }
        if (bytes.size() < 20)
        {
            fail(diagnostics, "import.source_malformed", "truncated GLB header");
            return false;
        }
        const std::uint32_t chunk_len = read_u32le(bytes, 12);
        const std::string_view chunk_type = bytes.substr(16, 4);
        if (chunk_type != "JSON")
        {
            fail(diagnostics, "import.source_malformed", "GLB first chunk is not JSON");
            return false;
        }
        const std::size_t json_at = 20;
        if (bytes.size() - json_at < static_cast<std::size_t>(chunk_len))
        {
            fail(diagnostics, "import.source_malformed", "truncated GLB JSON chunk");
            return false;
        }
        json_bytes = bytes.substr(json_at, chunk_len);
    }
    else
    {
        info.is_glb = false;
        json_bytes = bytes;
    }

    // Reuse the engine's canonical JSON parser — no third-party glTF library (deny-by-default deps).
    const serializer::CanonicalizeResult parsed = serializer::canonicalize(json_bytes);
    if (!parsed.is_json || parsed.root.type != JsonValue::Type::object)
    {
        fail(diagnostics, "import.decode_failed", "glTF root is not a JSON object");
        return false;
    }
    const JsonValue& root = parsed.root;

    const JsonValue* asset = member(root, "asset");
    if (asset == nullptr || asset->type != JsonValue::Type::object)
    {
        fail(diagnostics, "import.source_malformed", "glTF has no asset object");
        return false;
    }
    const JsonValue* version = member(*asset, "version");
    if (version == nullptr || version->type != JsonValue::Type::string)
    {
        fail(diagnostics, "import.source_malformed", "glTF has no asset.version string");
        return false;
    }
    info.version = version->string_value;
    if (info.version != "2.0")
    {
        fail(diagnostics, "import.unsupported_format",
             "glTF version " + detail::ascii_or_hex(info.version) + " unsupported (v1 supports 2.0)");
        return false;
    }

    const JsonValue* accessors = member(root, "accessors");
    const bool has_accessors = accessors != nullptr && accessors->type == JsonValue::Type::array;
    info.accessor_count = has_accessors ? accessors->elements.size() : 0;
    const JsonValue* materials = member(root, "materials");
    info.material_count = (materials != nullptr && materials->type == JsonValue::Type::array)
                              ? materials->elements.size()
                              : 0;

    const JsonValue* meshes = member(root, "meshes");
    if (meshes != nullptr && meshes->type == JsonValue::Type::array)
    {
        for (const JsonValue& mesh_value : meshes->elements)
        {
            GltfMeshInfo mesh;
            const JsonValue* name = member(mesh_value, "name");
            if (name != nullptr && name->type == JsonValue::Type::string)
                mesh.name = name->string_value;
            const JsonValue* primitives = member(mesh_value, "primitives");
            if (primitives != nullptr && primitives->type == JsonValue::Type::array)
            {
                for (const JsonValue& prim_value : primitives->elements)
                {
                    GltfPrimitiveInfo prim;
                    const JsonValue* attrs = member(prim_value, "attributes");
                    if (attrs != nullptr && attrs->type == JsonValue::Type::object)
                    {
                        prim.has_position = member(*attrs, "POSITION") != nullptr;
                        prim.has_normal = member(*attrs, "NORMAL") != nullptr;
                        prim.has_tangent = member(*attrs, "TANGENT") != nullptr;
                        prim.has_texcoord0 = member(*attrs, "TEXCOORD_0") != nullptr;
                        prim.has_texcoord1 = member(*attrs, "TEXCOORD_1") != nullptr;
                    }
                    prim.uv2_reserved = true; // R-REND-006 hook — reserved regardless of source

                    // `indices` is an ACCESSOR index; the element count lives on that accessor.
                    const std::int64_t indices_ref = as_int64(member(prim_value, "indices"), -1);
                    if (indices_ref >= 0 && has_accessors &&
                        static_cast<std::size_t>(indices_ref) < accessors->elements.size())
                    {
                        prim.index_count = as_int64(
                            member(accessors->elements[static_cast<std::size_t>(indices_ref)], "count"),
                            -1);
                    }
                    prim.material = as_int64(member(prim_value, "material"), -1);
                    mesh.primitives.push_back(prim);
                }
            }
            info.meshes.push_back(std::move(mesh));
        }
    }
    return true;
}

std::uint32_t GltfImporter::derived_format_version(ArtifactKind kind) const noexcept
{
    return kind == ArtifactKind::mesh ? 1U : 0U;
}

ImportResult GltfImporter::import(const ImportInput& input) const
{
    ImportResult result;
    GltfInfo info;
    if (!parse_gltf(input.source_bytes, info, result.diagnostics))
    {
        result.ok = false;
        return result;
    }

    JsonValue desc = jobject();
    put(desc, "kind", jstr("mesh"));
    put(desc, "source", jstr("gltf"));
    put(desc, "gltfVersion", jstr(info.version));
    put(desc, "container", jstr(info.is_glb ? "glb" : "gltf"));
    put(desc, "accessorCount", juint(info.accessor_count));
    put(desc, "materialCount", juint(info.material_count));
    put(desc, "meshCount", juint(info.meshes.size()));

    JsonValue meshes = jarray();
    bool any_uv2_populated = false;
    for (const GltfMeshInfo& mesh : info.meshes)
    {
        JsonValue mesh_object = jobject();
        put(mesh_object, "name", jstr(mesh.name));
        JsonValue primitives = jarray();
        for (const GltfPrimitiveInfo& prim : mesh.primitives)
        {
            JsonValue prim_object = jobject();
            put(prim_object, "position", jbool(prim.has_position));
            put(prim_object, "normal", jbool(prim.has_normal));
            put(prim_object, "tangent", jbool(prim.has_tangent));
            put(prim_object, "texcoord0", jbool(prim.has_texcoord0));
            // The reserved UV2 lightmap channel (R-REND-006): ALWAYS present in the descriptor,
            // populated only when the source authored TEXCOORD_1.
            JsonValue uv2 = jobject();
            put(uv2, "reserved", jbool(prim.uv2_reserved));
            put(uv2, "populated", jbool(prim.has_texcoord1));
            put(prim_object, "uv2", std::move(uv2));
            put(prim_object, "indexCount", jint(prim.index_count));
            put(prim_object, "material", jint(prim.material));
            append(primitives, std::move(prim_object));
            if (prim.has_texcoord1)
                any_uv2_populated = true;
        }
        put(mesh_object, "primitives", std::move(primitives));
        append(meshes, std::move(mesh_object));
    }
    put(desc, "meshes", std::move(meshes));

    // Top-level UV2 reservation summary (R-REND-006): the format contract carries the channel
    // whether or not any source populated it, so the M4 lightmap baker has a stable hook.
    JsonValue uv2_reservation = jobject();
    put(uv2_reservation, "channel", jstr("TEXCOORD_1"));
    put(uv2_reservation, "reserved", jbool(true));
    put(uv2_reservation, "anyPopulated", jbool(any_uv2_populated));
    put(desc, "uv2Reservation", std::move(uv2_reservation));

    JsonValue transcode = jobject();
    put(transcode, "platform", jstr(input.platform.id));
    const TranscodeTarget* target = transcode_target_for(ArtifactKind::mesh, input.platform.id);
    put(transcode, "format", jstr(target != nullptr ? target->format : std::string("unmapped")));
    put(desc, "transcode", std::move(transcode));

    std::string canonical;
    if (!serializer::serialize_canonical(desc, canonical))
    {
        result.ok = false;
        result.diagnostics.push_back(
            {"import.decode_failed", "failed to serialize the mesh descriptor"});
        return result;
    }

    DerivedArtifact artifact;
    artifact.kind = ArtifactKind::mesh;
    artifact.name = "mesh";
    artifact.bytes = std::move(canonical);
    artifact.derived_format_version = derived_format_version(ArtifactKind::mesh);
    result.artifacts.push_back(std::move(artifact));
    result.ok = true;
    return result;
}

} // namespace context::editor::import
