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
#include "support/TempPath.h"
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
        fx.Dir = Veng::TestSupport::TempDir() / fmt::format("veng_graph_cook_{}", name);
        std::filesystem::remove_all(fx.Dir);
        std::filesystem::create_directories(fx.Dir);

        WriteFile(fx.Dir / "fullscreen.vert.slang", FullscreenVert);
        WriteFile(fx.Dir / "fullscreen.vert.shader.json",
                  R"({ "source": "fullscreen.vert.slang", "entry": "vsMain" })");

        WriteFile(fx.Dir / "color.frag.graph.json", graphDoc);
        WriteFile(
            fx.Dir / "color.frag.shader.json",
            R"({ "source": "color.frag.graph.json", "entry": "fsMain", "domain": "PostProcess" })");

        // The material's .vmat field list is hand-authored here to match the generated
        // MaterialParams; the editor regenerates it from the same walk. A const-param graph
        // generates no field, so the default list is empty.
        WriteFile(fx.Dir / "color.vmat.json", R"({
  "domain": "PostProcess",
  "shaders": { "vertex": "0x0000000000002261", "fragment": "0x0000000000002262" },
  "fields": []
})");

        WriteFile(fx.Dir / "pack.json", R"({
  "version": 1,
  "assets": [
    { "id": "0x0000000000002261", "type": "Shader",   "source": "fullscreen.vert.shader.json" },
    { "id": "0x0000000000002262", "type": "Shader",   "source": "color.frag.shader.json" },
    { "id": "0x0000000000002263", "type": "Material", "source": "color.vmat.json" }
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
  "domain": "PostProcess",
  "shaders": { "vertex": "0x0000000000002261", "fragment": "0x0000000000002262" },
  "fields": [ { "name": "n1", "type": "vec4", "value": [0.2, 0.6, 0.9, 1.0] } ]
})");

    const Result<ArchiveReader> reader = CookFixture(fx);
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    // The generated fragment shader cooked to a real shader blob.
    const optional<ArchiveEntry> shader = reader->Find(AssetId{8802});
    REQUIRE(shader.has_value());
    CHECK(shader->Type == AssetTypes::Shader);

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
    CHECK(shader->Type == AssetTypes::Shader);

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

// --- GuiFill: a UI-fill graph cooks against the gui vertex interface -------------------
//
// The GuiFill domain wraps an authored graph against the engine's gui vertex stage rather
// than a fullscreen triangle: the generated fragment reads the gui interpolants, resolves
// through the engine's rounded-rect coverage, and reserves the GUI pass's push block ahead
// of its material selector. Cooking one proves the generated Slang compiles, the vertex
// stage's reflected inputs validate against the gui vertex layout (a ShaderImporter hard
// error otherwise), and the reflected push range covers the selector at the offset
// MaterialLoader's SelectorPushOffsetFor demands.

namespace
{
    // The gui vertex stage, matching the engine's core gui.vert contract: a
    // framebuffer-pixel position through the reserved push block's inverse screen size,
    // forwarding the interpolants a fill reads.
    constexpr std::string_view GuiVert = R"(#include "Veng/guifill.slang"
struct VSInput
{
    float2 a_Position : POSITION;
    float2 a_Uv : TEXCOORD0;
    float4 a_Color : COLOR0;
    float2 a_RectHalf : TEXCOORD1;
    float2 a_RectCoord : TEXCOORD2;
    float4 a_Params : TEXCOORD3;
    uint a_GradientSelector : TEXCOORD4;
    float4 a_Shadow : TEXCOORD5;
};

[shader("vertex")]
GuiFillInputs vsMain(VSInput input)
{
    GuiFillInputs output;
    float2 ndc = input.a_Position * g_PC.InvScreenSize * 2.0 - 1.0;
    output.sv_position = float4(ndc, 0.0, 1.0);
    output.v_UV = input.a_Uv;
    output.v_Color = input.a_Color;
    output.v_RectHalf = input.a_RectHalf;
    output.v_RectCoord = input.a_RectCoord;
    output.v_Params = input.a_Params;
    return output;
}
)";

    // A GuiFill graph animating an exposed tint: Multiply(tint, GuiTime) → Color. The scalar
    // time splats across the vec4 through the link coercion, so the fill pulses without any
    // per-frame game code — and the walk still emits a MaterialParams the cook reflects.
    string MakeGuiFillGraph(vec4 tint)
    {
        NodeCatalog catalog;
        MaterialEmitTable emit;
        const MaterialNodeTypes types =
            RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::GuiFill);

        NodeGraph graph(
            MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id)
            {
                const NodeType* type = catalog.Find(id);
                return type != nullptr ? type->PropertySize : usize{0};
            });

        const NodeType* time = catalog.Find(GuiTimeTypeName);
        const NodeType* multiply = catalog.Find(MultiplyTypeName);
        REQUIRE(time != nullptr);
        REQUIRE(multiply != nullptr);

        const NodeId output = graph.AddNode(types.MaterialOutput);
        const NodeId param = graph.AddNode(types.Param);
        const NodeId timeNode = graph.AddNode(time->Id);
        const NodeId mulNode = graph.AddNode(multiply->Id);

        const NodeType* paramType = catalog.Find(types.Param);
        REQUIRE(paramType != nullptr);
        const ParamProvenance provenance = ParamProvenance::Exposed;
        for (const FieldDescriptor& field : paramType->Properties)
        {
            if (field.Name == ParamValueProperty)
            {
                graph.SetProperty(param, field,
                                  std::span<const std::byte>(
                                      reinterpret_cast<const std::byte*>(&tint), sizeof(tint)));
            }
            else if (field.Name == ParamProvenanceProperty)
            {
                graph.SetProperty(
                    param, field,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(&provenance),
                                               sizeof(provenance)));
            }
        }

        REQUIRE(graph.Connect(PinRef{.Node = param, .Pin = 0}, PinRef{.Node = mulNode, .Pin = 0})
                    .has_value());
        REQUIRE(graph.Connect(PinRef{.Node = timeNode, .Pin = 0}, PinRef{.Node = mulNode, .Pin = 1})
                    .has_value());
        REQUIRE(graph.Connect(PinRef{.Node = mulNode, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                    .has_value());

        return WriteNodeGraph(graph, catalog);
    }

    GraphFixture WriteGuiFillPack(const string& graphDoc)
    {
        GraphFixture fx;
        fx.Dir = Veng::TestSupport::TempDir() / "veng_graph_cook_guifill";
        std::filesystem::remove_all(fx.Dir);
        std::filesystem::create_directories(fx.Dir);

        // The gui vertex layout the vertex stage's reflected inputs are validated against.
        WriteFile(fx.Dir / "gui.vlayout.json", R"({
  "elements": [
    { "format": "RG32Sfloat",   "name": "a_Position" },
    { "format": "RG32Sfloat",   "name": "a_Uv" },
    { "format": "RGBA32Sfloat", "name": "a_Color" },
    { "format": "RG32Sfloat",   "name": "a_RectHalf" },
    { "format": "RG32Sfloat",   "name": "a_RectCoord" },
    { "format": "RGBA32Sfloat", "name": "a_Params" },
    { "format": "R32Uint",      "name": "a_GradientSelector" },
    { "format": "RGBA32Sfloat", "name": "a_Shadow" }
  ]
})");

        WriteFile(fx.Dir / "gui.vert.slang", GuiVert);
        WriteFile(fx.Dir / "gui.vert.shader.json",
                  R"({ "source": "gui.vert.slang", "entry": "vsMain",
                       "vertex_layout": "0x0000000000002270" })");

        WriteFile(fx.Dir / "fill.frag.graph.json", graphDoc);
        WriteFile(
            fx.Dir / "fill.frag.shader.json",
            R"({ "source": "fill.frag.graph.json", "entry": "fsMain", "domain": "GuiFill" })");

        WriteFile(fx.Dir / "fill.vmat.json", R"({
  "domain": "GuiFill",
  "shaders": { "vertex": "0x0000000000002271", "fragment": "0x0000000000002272" },
  "fields": [ { "name": "n1", "type": "vec4", "value": [0.9, 0.3, 0.1, 1.0] } ]
})");

        WriteFile(fx.Dir / "pack.json", R"({
  "version": 1,
  "assets": [
    { "id": "0x0000000000002270", "type": "VertexLayout", "source": "gui.vlayout.json" },
    { "id": "0x0000000000002271", "type": "Shader",       "source": "gui.vert.shader.json" },
    { "id": "0x0000000000002272", "type": "Shader",       "source": "fill.frag.shader.json" },
    { "id": "0x0000000000002273", "type": "Material",     "source": "fill.vmat.json" }
  ]
})");

        fx.PackJson = fx.Dir / "pack.json";
        fx.Archive = fx.Dir / "out.vengpack";
        return fx;
    }
}

TEST_CASE("Cooker: a GuiFill graph cooks against the gui vertex interface and push contract")
{
    const string graphDoc = MakeGuiFillGraph(vec4(0.9f, 0.3f, 0.1f, 1.0f));
    const GraphFixture fx = WriteGuiFillPack(graphDoc);

    const Result<ArchiveReader> reader = CookFixture(fx);
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    // The vertex stage cooked, and its reflected inputs validated against the gui vertex
    // layout — the ShaderImporter hard-errors on a mismatch, so the recorded layout id is
    // the proof the generated fill rides the real gui vertex interface.
    const optional<ArchiveEntry> vert = reader->Find(AssetId{0x2271});
    REQUIRE(vert.has_value());
    CookedShaderHeader vertHeader{};
    std::memcpy(&vertHeader, vert->Blob.data(), sizeof(vertHeader));
    CookedShaderInterfaceHeader vertInterface{};
    std::memcpy(&vertInterface, vert->Blob.data() + sizeof(vertHeader), sizeof(vertInterface));
    CHECK(vertInterface.VertexLayoutAssetId == 0x2270ULL);

    // The generated fragment cooked, and its reflected push range covers the material
    // selector at the domain's declared offset — what MaterialLoader's SelectorPushOffsetFor
    // requires before it will build a GuiFill pipeline layout.
    const optional<ArchiveEntry> frag = reader->Find(AssetId{0x2272});
    REQUIRE(frag.has_value());
    CookedShaderHeader fragHeader{};
    std::memcpy(&fragHeader, frag->Blob.data(), sizeof(fragHeader));
    CookedShaderInterfaceHeader fragInterface{};
    std::memcpy(&fragInterface, frag->Blob.data() + sizeof(fragHeader), sizeof(fragInterface));
    REQUIRE(fragInterface.PushConstantCount >= 1);

    const auto* blocks = reinterpret_cast<const CookedPushConstantBlock*>(
        frag->Blob.data() + sizeof(fragHeader) + sizeof(fragInterface) +
        fragInterface.BindingCount * sizeof(CookedDescriptorBinding));
    bool selectorCovered = false;
    for (u32 i = 0; i < fragInterface.PushConstantCount; ++i)
    {
        if (blocks[i].Offset <= GuiFillSelectorPushOffset &&
            blocks[i].Offset + blocks[i].Size >= GuiFillSelectorPushOffset + sizeof(u32))
        {
            selectorCovered = true;
        }
    }
    CHECK(selectorCovered);

    // The material resolves to the pass-built GuiFill domain and packs the exposed tint.
    const optional<ArchiveEntry> mat = reader->Find(AssetId{0x2273});
    REQUIRE(mat.has_value());
    CookedMaterialHeader header{};
    std::memcpy(&header, mat->Blob.data(), sizeof(header));
    CHECK(header.VertexShaderId == 0x2271ULL);
    CHECK(header.FragmentShaderId == 0x2272ULL);
    CHECK(header.Domain == static_cast<u32>(MaterialDomain::GuiFill));
    CHECK(header.FieldCount == 1);

    std::filesystem::remove_all(fx.Dir);
}
