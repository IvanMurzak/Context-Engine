// glTF importer: JSON + GLB parse, mesh/primitive attributes, the R-REND-006 UV2 reservation,
// failure paths.

#include "context/editor/import/import_settings.h"
#include "context/editor/import/importers/gltf_importer.h"
#include "context/editor/import/platform_profile.h"

#include "descriptor_read.h"
#include "import_test.h"

#include <string>
#include <vector>

using namespace context::editor::import;
using context::editor::serializer::JsonValue;

namespace
{
bool has_code(const std::vector<ImportDiagnostic>& diags, const char* code)
{
    for (const ImportDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}
} // namespace

int main()
{
    const GltfImporter importer;
    const PlatformProfile win = *find_platform_profile("windows");
    const ImportSettings settings = resolve_import_settings("", "windows");

    // Happy path: a .gltf triangle without a lightmap UV -> UV2 RESERVED but not populated.
    {
        const std::string gltf = importtest::make_gltf_json(/*with_uv2=*/false);
        GltfInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_gltf(gltf, info, diags));
        CHECK(diags.empty());
        CHECK(!info.is_glb);
        CHECK(info.version == "2.0");
        CHECK(info.meshes.size() == 1);
        CHECK(info.meshes[0].name == "Tri");
        CHECK(info.meshes[0].primitives.size() == 1);
        const GltfPrimitiveInfo& prim = info.meshes[0].primitives[0];
        CHECK(prim.has_position);
        CHECK(prim.has_normal);
        CHECK(prim.has_texcoord0);
        CHECK(!prim.has_texcoord1);
        CHECK(prim.uv2_reserved);       // reserved regardless (R-REND-006 hook)
        CHECK(prim.index_count == 3);   // resolved from the indices accessor's count
        CHECK(prim.material == 0);

        ImportInput in;
        in.source_path = "tri.gltf";
        in.source_bytes = gltf;
        in.settings = settings;
        in.platform = win;
        const ImportResult r = importer.import(in);
        CHECK(r.ok);
        CHECK(r.artifacts.size() == 1);
        CHECK(r.artifacts[0].kind == ArtifactKind::mesh);

        const auto desc = importtest::parse_descriptor(r.artifacts[0].bytes);
        CHECK(importtest::sfield(desc, "kind") == "mesh");
        CHECK(importtest::sfield(desc, "container") == "gltf");
        CHECK(importtest::ifield(desc, "meshCount") == 1);
        const JsonValue* uv2 = importtest::ofield(desc, "uv2Reservation");
        CHECK(uv2 != nullptr);
        CHECK(importtest::bfield(*uv2, "reserved"));      // ALWAYS reserved
        CHECK(!importtest::bfield(*uv2, "anyPopulated")); // none authored here
        CHECK(importtest::sfield(*uv2, "channel") == "TEXCOORD_1");
    }

    // A source WITH TEXCOORD_1 populates the reserved UV2 channel.
    {
        const std::string gltf = importtest::make_gltf_json(/*with_uv2=*/true);
        GltfInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_gltf(gltf, info, diags));
        CHECK(info.meshes[0].primitives[0].has_texcoord1);

        ImportInput in;
        in.source_path = "lm.gltf";
        in.source_bytes = gltf;
        in.settings = settings;
        in.platform = win;
        const auto desc = importtest::parse_descriptor(importer.import(in).artifacts[0].bytes);
        CHECK(importtest::bfield(*importtest::ofield(desc, "uv2Reservation"), "anyPopulated"));
    }

    // GLB binary container unwraps to the same mesh (is_glb flagged).
    {
        const std::string glb = importtest::make_glb(importtest::make_gltf_json(false));
        GltfInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_gltf(glb, info, diags));
        CHECK(info.is_glb);
        CHECK(info.meshes.size() == 1);

        ImportInput in;
        in.source_path = "tri.glb";
        in.source_bytes = glb;
        in.settings = settings;
        in.platform = win;
        const auto desc = importtest::parse_descriptor(importer.import(in).artifacts[0].bytes);
        CHECK(importtest::sfield(desc, "container") == "glb");
    }

    // Failure paths.
    {
        GltfInfo info;
        std::vector<ImportDiagnostic> diags;

        CHECK(!parse_gltf("this is not json", info, diags));
        CHECK(has_code(diags, "import.decode_failed"));

        diags.clear();
        CHECK(!parse_gltf("{\"asset\":{\"version\":\"1.0\"}}", info, diags)); // unsupported version
        CHECK(has_code(diags, "import.unsupported_format"));

        diags.clear();
        CHECK(!parse_gltf("{\"meshes\":[]}", info, diags)); // no asset object
        CHECK(has_code(diags, "import.source_malformed"));

        diags.clear();
        CHECK(!parse_gltf("[]", info, diags)); // root not an object
        CHECK(has_code(diags, "import.decode_failed"));
    }

    IMPORT_TEST_MAIN_END();
}
