// Cook-through: the editor's CompileMaterialGraph output, serialized to a .vmat
// by WriteMaterialVmat, cooks unchanged through the real MaterialImporter (Slang
// present). This pins that the emitted JSON — texture+sampler pairing and vecN
// values — is exactly what the importer accepts, not merely an equivalent array.
//
// The test builds the brick material's graph (BuildGraphFromMaterial over the
// brick field table), patches the Factors param, compiles, serializes, writes
// the .vmat plus a temp pack referencing the fixture shaders/texture, and cooks.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/TypeId.h>

#include "material/MaterialCatalog.h"
#include "material/MaterialCompile.h"
#include "material/MaterialShaderInterface.h"

using namespace Veng;
using namespace Veng::Cook;
using namespace VengEditor;

namespace
{
    const path FixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);

    using Field = MaterialField;
    using Kind = MaterialField::FieldKind;

    Veng::vector<Field> BrickFields()
    {
        return {
            Field{.Name = "Albedo", .Offset = 0, .Size = 4, .Kind = Kind::TextureHandle, .TextureId = 2001},
            Field{.Name = "AlbedoSampler", .Offset = 4, .Size = 4, .Kind = Kind::SamplerHandle, .TextureId = 2001},
            Field{.Name = "Factors", .Offset = 0, .Size = 16, .Kind = Kind::Param},
        };
    }
}

TEST_CASE("Cooker: a compiled material graph cooks through the real MaterialImporter")
{
    // --- 1. Build + compile a brick-equivalent graph (editor layers 2/3) ---

    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{
        .Fields = brick,
        .VertexShader = AssetId{4101},
        .FragmentShader = AssetId{4102},
    };

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    // Patch the Factors param so the cooked authored block carries real values.
    NodeId paramNode{};
    for (NodeId node : graph.Nodes())
        if (graph.GetTypeOf(node) == types.Param)
            paramNode = node;
    REQUIRE(graph.IsValid(paramNode));

    const NodeType* paramType = catalog.Find(types.Param);
    REQUIRE(paramType != nullptr);
    const vec4 factors{1.0f, 0.9f, 0.8f, 1.0f};
    const std::byte* factorBytes = reinterpret_cast<const std::byte*>(&factors);
    graph.SetProperty(paramNode, paramType->Properties[0],
                      std::span<const std::byte>(factorBytes, sizeof(factors)));

    const Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::Surface);
    REQUIRE(compiled.has_value());

    const string vmatJson = WriteMaterialVmat(*compiled, iface, MaterialDomain::Surface);

    // --- 2. Write the emitted .vmat + a temp pack into the fixture tree ---

    const path vmatPath = FixtureDir / "materials" / "compiled.vmat.json";
    {
        std::ofstream vmatFile(vmatPath, std::ios::binary | std::ios::trunc);
        REQUIRE(vmatFile.is_open());
        vmatFile << vmatJson;
    }

    const path packPath = FixtureDir / "material_compiled_pack.json";
    {
        std::ofstream packFile(packPath, std::ios::binary | std::ios::trunc);
        REQUIRE(packFile.is_open());
        packFile <<
            R"({
  "version": 1,
  "assets": [
    { "id": 7001, "type": "vertex_layout", "source": "layouts/canonical.vlayout.json" },
    { "id": 4101, "type": "shader", "source": "shaders/brick.vert.shader.json" },
    { "id": 4102, "type": "shader", "source": "shaders/brick.frag.shader.json" },
    { "id": 2001, "type": "texture", "source": "textures/texture.tex.json" },
    { "id": 9001, "type": "material", "source": "materials/compiled.vmat.json" }
  ]
})";
    }

    // --- 3. Cook through the real importer + validate the cooked blob ---

    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_compiled.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cookResult = cooker.CookPack(packPath, outArchive);
    if (!cookResult.has_value())
        FAIL("cook failed: ", cookResult.error());
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{9001});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Material);

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.VertexShaderId == 4101ULL);
    CHECK(header.FragmentShaderId == 4102ULL);
    CHECK(header.FieldCount == 3); // Albedo + AlbedoSampler + Factors

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    const CookedMaterialField* albedo = nullptr;
    const CookedMaterialField* sampler = nullptr;
    const CookedMaterialField* params = nullptr;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        const std::string_view name(fieldTable[i].Name);
        if (name == "Albedo") albedo = &fieldTable[i];
        else if (name == "AlbedoSampler") sampler = &fieldTable[i];
        else if (name == "Factors") params = &fieldTable[i];
    }
    REQUIRE(albedo != nullptr);
    REQUIRE(sampler != nullptr);
    REQUIRE(params != nullptr);

    CHECK(albedo->Kind == 1u); // texture handle
    CHECK(albedo->TextureId == 2001ULL);
    CHECK(sampler->Kind == 2u); // sampler handle, paired to Albedo's texture
    CHECK(sampler->TextureId == 2001ULL);
    CHECK(params->Kind == 0u);  // authored param

    // The param block carries the four f32s the graph compiled, at Factors' offset.
    const u8* block = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + header.FieldCount * sizeof(CookedMaterialField);
    f32 cookedFactors[4];
    std::memcpy(cookedFactors, block + params->Offset, sizeof(cookedFactors));
    CHECK(cookedFactors[0] == doctest::Approx(1.0f));
    CHECK(cookedFactors[1] == doctest::Approx(0.9f));
    CHECK(cookedFactors[2] == doctest::Approx(0.8f));
    CHECK(cookedFactors[3] == doctest::Approx(1.0f));

    std::filesystem::remove(outArchive);
    std::filesystem::remove(vmatPath);
    std::filesystem::remove(packPath);
}
