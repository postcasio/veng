// Graph-sourced shader cook test: authors a node graph, writes it as a fragment
// shader's source (a *.graph.json named by a *.shader.json), cooks the pack
// offline through libveng_cook, and asserts the cooked shader + material blobs.
// This is the offline half of the graph→cook→reload loop: the same emit walk the
// editor preview runs (veng::graph) generates the Slang the cooker compiles, so
// the cooked material binds the generated fragment and packs the exposed param at
// the cooker-reflected offset.
//
// A const Param folds inline (no field); an exposed Param contributes a field with
// its authored default; an engine-bound Param a field with no default. The cooker
// reflects the generated MaterialParams and validates the generated .vmat field
// list against it, exactly as a hand-authored material/shader pair.

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

#include <VengGraph/MaterialCatalog.h>
#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeGraphSerialize.h>
#include <VengGraph/NodeType.h>

using namespace Veng;
using namespace Veng::Cook;
using namespace VengGraph;

namespace
{
    // A self-contained pack written into a temp dir: a fullscreen vertex shader, a
    // graph-sourced PostProcess fragment shader, and a material referencing both.
    struct GraphFixture
    {
        path Dir;
        path PackJson;
        path Archive;
    };

    void WriteFile(const path& p, std::string_view contents)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    // A PostProcess fullscreen vertex stage (no vertex inputs, so no vertex layout
    // reference is needed for the pack to be self-contained).
    constexpr std::string_view FullscreenVert = R"(
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain()
{
    int vertexID = spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); };
    VSOutput output;
    output.v_UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.sv_position = float4(output.v_UV * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
)";

    // Builds a PostProcess material graph: a single exposed/const Param feeds the Color
    // sink, returning its serialized form.
    string MakeColorGraph(ParamProvenance provenance, vec4 value)
    {
        NodeCatalog catalog;
        MaterialEmitTable emit;
        const MaterialNodeTypes types =
            RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::PostProcess);

        NodeGraph graph(
            MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id)
            {
                const NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : usize{0};
            });

        const NodeId output = graph.AddNode(types.MaterialOutput);
        const NodeId param = graph.AddNode(types.Param);

        // The Param property POD is { vec4 Value; ParamProvenance Provenance; }; write both
        // through the catalog's reflected field descriptors.
        const NodeType* paramType = catalog.Find(types.Param);
        REQUIRE(paramType != nullptr);
        for (const FieldDescriptor& field : paramType->Properties)
        {
            if (field.Name == ParamValueProperty)
            {
                graph.SetProperty(param, field,
                                  std::span<const std::byte>(
                                      reinterpret_cast<const std::byte*>(&value), sizeof(value)));
            }
            else if (field.Name == ParamProvenanceProperty)
            {
                graph.SetProperty(
                    param, field,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(&provenance),
                                               sizeof(provenance)));
            }
        }

        REQUIRE(graph.Connect(PinRef{.Node = param, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                    .has_value());

        return WriteNodeGraph(graph, catalog);
    }

    // Builds a PostProcess graph mixing the math/swizzle/utility nodes: a Constant tint
    // (vec4) and a Constant scalar are combined and multiplied through a Combine → Multiply
    // chain whose components are split and re-packed, then fed to the Color sink. Everything
    // is const-folded, so the generated MaterialParams is empty and the cook only proves the
    // generated Slang compiles.
    string MakeMathGraph()
    {
        NodeCatalog catalog;
        MaterialEmitTable emit;
        const MaterialNodeTypes types =
            RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::PostProcess);

        NodeGraph graph(
            MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id)
            {
                const NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : usize{0};
            });

        const auto find = [&](const char* name)
        {
            const NodeType* type = catalog.Find(name);
            REQUIRE(type != nullptr);
            return type;
        };
        const auto setVec4 = [&](NodeId node, const NodeType* type, const vec4& v)
        {
            const FieldDescriptor& field = type->Properties[0];
            graph.SetProperty(
                node, field,
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(&v), sizeof(v)));
            // Author the leaf type as Vec4.
            const MaterialLeafType leaf = MaterialLeafType::Vec4;
            graph.SetProperty(node, type->Properties[1],
                              std::span<const std::byte>(reinterpret_cast<const std::byte*>(&leaf),
                                                         sizeof(leaf)));
        };

        const NodeType* constType = find(ConstantTypeName);
        const NodeType* split = find(SplitTypeName);
        const NodeType* combine = find(CombineTypeName);
        const NodeType* multiply = find(MultiplyTypeName);
        const NodeType* saturate = find(SaturateTypeName);

        const NodeId output = graph.AddNode(types.MaterialOutput);
        const NodeId tint = graph.AddNode(constType->Id);
        const NodeId scale = graph.AddNode(constType->Id);
        const NodeId splitNode = graph.AddNode(split->Id);
        const NodeId combineNode = graph.AddNode(combine->Id);
        const NodeId mulNode = graph.AddNode(multiply->Id);
        const NodeId satNode = graph.AddNode(saturate->Id);

        setVec4(tint, constType, vec4(0.8f, 0.4f, 0.2f, 1.0f));
        setVec4(scale, constType, vec4(0.5f, 0.5f, 0.5f, 1.0f));

        // tint → split; split.x/y/z/w → combine.x/y/z/w (round-trip the channels);
        // combine → mul.A, scale → mul.B; mul → saturate → Color.
        REQUIRE(graph.Connect(PinRef{.Node = tint, .Pin = 0}, PinRef{.Node = splitNode, .Pin = 0})
                    .has_value());
        for (u16 i = 0; i < 4; ++i)
        {
            REQUIRE(graph
                        .Connect(PinRef{.Node = splitNode, .Pin = i},
                                 PinRef{.Node = combineNode, .Pin = i})
                        .has_value());
        }
        REQUIRE(
            graph.Connect(PinRef{.Node = combineNode, .Pin = 0}, PinRef{.Node = mulNode, .Pin = 0})
                .has_value());
        REQUIRE(graph.Connect(PinRef{.Node = scale, .Pin = 0}, PinRef{.Node = mulNode, .Pin = 1})
                    .has_value());
        REQUIRE(graph.Connect(PinRef{.Node = mulNode, .Pin = 0}, PinRef{.Node = satNode, .Pin = 0})
                    .has_value());
        REQUIRE(graph.Connect(PinRef{.Node = satNode, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                    .has_value());

        return WriteNodeGraph(graph, catalog);
    }

    // Writes the pack files (vertex .slang/.shader.json, graph .graph.json/.shader.json,
    // material .vmat.json, manifest) into a fresh temp dir.
    GraphFixture WritePack(const string& name, const string& graphDoc)
    {
        GraphFixture fx;
        fx.Dir = std::filesystem::temp_directory_path() / fmt::format("veng_graph_cook_{}", name);
        std::filesystem::remove_all(fx.Dir);
        std::filesystem::create_directories(fx.Dir);

        WriteFile(fx.Dir / "fullscreen.vert.slang", FullscreenVert);
        WriteFile(fx.Dir / "fullscreen.vert.shader.json",
                  R"({ "source": "fullscreen.vert.slang", "entry": "vsMain" })");

        WriteFile(fx.Dir / "color.frag.graph.json", graphDoc);
        WriteFile(
            fx.Dir / "color.frag.shader.json",
            R"({ "source": "color.frag.graph.json", "entry": "fsMain", "domain": "postprocess" })");

        // The material's .vmat field list is hand-authored here to match the generated
        // MaterialParams; the editor regenerates it from the same walk. A const-param graph
        // generates no field, so the default list is empty.
        WriteFile(fx.Dir / "color.vmat.json", R"({
  "domain": "postprocess",
  "shaders": { "vertex": 8801, "fragment": 8802 },
  "fields": []
})");

        WriteFile(fx.Dir / "pack.json", R"({
  "version": 1,
  "assets": [
    { "id": 8801, "type": "shader",   "source": "fullscreen.vert.shader.json" },
    { "id": 8802, "type": "shader",   "source": "color.frag.shader.json" },
    { "id": 8803, "type": "material", "source": "color.vmat.json" }
  ]
})");

        fx.PackJson = fx.Dir / "pack.json";
        fx.Archive = fx.Dir / "out.vengpack";
        return fx;
    }

    Result<ArchiveReader> CookFixture(const GraphFixture& fx)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        // The generated fragment `#include`s the engine header; thread the core shader dir.
        const VoidResult cooked = cooker.CookPack(fx.PackJson, fx.Archive, {}, nullptr, nullptr,
                                                  nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        return ArchiveReader::Open(fx.Archive);
    }
}

TEST_CASE("Cooker: an exposed Param graph cooks a generated fragment + a field with its default")
{
    // The material's .vmat carries no "fields"; the generated field list is what the cook
    // packs. For an offline cook the .vmat field list must declare the exposed param, so
    // author it to match the single generated MaterialParams field. The field name is the
    // graph's stable node key ("n1" — the Param is the second node created).
    const string graphDoc = MakeColorGraph(ParamProvenance::Exposed, vec4(0.2f, 0.6f, 0.9f, 1.0f));
    const GraphFixture fx = WritePack("exposed", graphDoc);

    // The generated struct's single field is named after the Param's creation-order key.
    WriteFile(fx.Dir / "color.vmat.json", R"({
  "domain": "postprocess",
  "shaders": { "vertex": 8801, "fragment": 8802 },
  "fields": [ { "name": "n1", "type": "vec4", "value": [0.2, 0.6, 0.9, 1.0] } ]
})");

    const Result<ArchiveReader> reader = CookFixture(fx);
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    // The generated fragment shader cooked to a real shader blob.
    const optional<ArchiveEntry> shader = reader->Find(AssetId{8802});
    REQUIRE(shader.has_value());
    CHECK(shader->Type == AssetType::Shader);

    // The material binds the generated fragment and packs the exposed default.
    const optional<ArchiveEntry> mat = reader->Find(AssetId{8803});
    REQUIRE(mat.has_value());
    REQUIRE(mat->Blob.size() >= sizeof(CookedMaterialHeader));

    CookedMaterialHeader header{};
    std::memcpy(&header, mat->Blob.data(), sizeof(header));
    CHECK(header.FragmentShaderId == 8802ULL);
    CHECK(header.Domain == static_cast<u32>(MaterialDomain::PostProcess));
    CHECK(header.FieldCount == 1);
    CHECK(header.BlockBytes >= 16); // one float4

    const auto* fields = reinterpret_cast<const CookedMaterialField*>(mat->Blob.data() +
                                                                      sizeof(CookedMaterialHeader));
    CHECK(std::string_view(fields[0].Name) == "n1");
    CHECK(fields[0].Kind == 0u); // param
    CHECK(fields[0].Size == 16u);

    // The authored default is packed at the reflected offset.
    const u8* block = mat->Blob.data() + sizeof(CookedMaterialHeader) + sizeof(CookedMaterialField);
    f32 packed[4];
    std::memcpy(packed, block + fields[0].Offset, sizeof(packed));
    CHECK(packed[0] == doctest::Approx(0.2f));
    CHECK(packed[1] == doctest::Approx(0.6f));
    CHECK(packed[2] == doctest::Approx(0.9f));
    CHECK(packed[3] == doctest::Approx(1.0f));

    std::filesystem::remove_all(fx.Dir);
}

TEST_CASE("Cooker: a graph mixing math/swizzle nodes cooks a fragment that compiles")
{
    // The whole chain is const-folded, so the generated MaterialParams is empty; the cook
    // succeeding is the proof that the emitted operators, swizzles, and constructors form
    // valid Slang that compiles through the ShaderImporter.
    const string graphDoc = MakeMathGraph();
    const GraphFixture fx = WritePack("math", graphDoc);

    const Result<ArchiveReader> reader = CookFixture(fx);
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    const optional<ArchiveEntry> shader = reader->Find(AssetId{8802});
    REQUIRE(shader.has_value());
    CHECK(shader->Type == AssetType::Shader);

    const optional<ArchiveEntry> mat = reader->Find(AssetId{8803});
    REQUIRE(mat.has_value());
    CookedMaterialHeader header{};
    std::memcpy(&header, mat->Blob.data(), sizeof(header));
    CHECK(header.FragmentShaderId == 8802ULL);
    CHECK(header.FieldCount == 0);

    std::filesystem::remove_all(fx.Dir);
}

TEST_CASE("Cooker: a const Param graph folds inline and produces no field")
{
    // A const Param emits its value as a Slang literal, so the generated MaterialParams is
    // empty and the material declares no field.
    const string graphDoc = MakeColorGraph(ParamProvenance::Const, vec4(0.5f, 0.5f, 0.5f, 1.0f));
    const GraphFixture fx = WritePack("const", graphDoc);

    const Result<ArchiveReader> reader = CookFixture(fx);
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    const optional<ArchiveEntry> mat = reader->Find(AssetId{8803});
    REQUIRE(mat.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, mat->Blob.data(), sizeof(header));
    CHECK(header.FragmentShaderId == 8802ULL);
    CHECK(header.FieldCount == 0);
    CHECK(header.BlockBytes == 0);

    std::filesystem::remove_all(fx.Dir);
}
