// Graph-material layout guard (GPU). The std140/scalar offset trap: the cooker
// reflects the generated MaterialParams under std140 (a float4 16-byte-aligned),
// while the shader reads it scalar through Load<MaterialParams> (a float4
// 4-byte-aligned). If the generator emitted a vec4 param *after* the uint texture
// handle slots, the two layouts would place it at different offsets and the
// runtime would write the authored color where the shader cannot read it — the
// material renders black. The emit walk orders fields large-alignment-first
// precisely to avoid this; this test renders a graph whose MaterialParams holds a
// vec4 param alongside uint handle slots and asserts the authored color reaches
// the albedo, the only check that exercises the invariant on the GPU rather than
// by an offset-equality assert.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/packing.hpp>

#include <VengGraph/MaterialCatalog.h>
#include <VengGraph/MaterialCompile.h>
#include <VengGraph/MaterialShaderInterface.h>
#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeGraphSerialize.h>
#include <VengGraph/NodeType.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;
using namespace VengGraph;

namespace
{
    // The exposed param's authored albedo: a known non-grey color so a correct read
    // is unambiguous and a mis-offset read (zero or a uint-handle bit pattern) is not.
    constexpr vec4 ParamAlbedo{0.2f, 0.6f, 0.85f, 1.0f};

    void WriteFile(const path& p, std::string_view contents)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "graph material: a vec4 param alongside uint handle slots renders its color "
                  "(std140/scalar offset guard)")
{
    RegisterBuiltinTypes(Types);

    // --- Author the Surface graph: an exposed vec4 Param → Albedo, a TextureSample → Normal.
    // The reached TextureSample contributes two uint handle slots to MaterialParams, so the
    // generated struct mixes a vec4 with uints — the exact layout the ordering guards. ---
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph(
        MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
        [&catalog](NodeTypeId id)
        {
            const NodeType* type = catalog.Find(id);
            return type != nullptr ? type->PropertySize : usize{0};
        });

    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId sample = graph.AddNode(types.TextureSample);
    const NodeId param = graph.AddNode(types.Param);

    // The Param is exposed with the known albedo as its default.
    {
        const ParamProvenance provenance = ParamProvenance::Exposed;
        const vec4 value = ParamAlbedo;
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
    }

    REQUIRE(graph.Connect(PinRef{.Node = param, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                .has_value()); // Param → Albedo
    REQUIRE(graph.Connect(PinRef{.Node = sample, .Pin = 0}, PinRef{.Node = output, .Pin = 1})
                .has_value()); // TextureSample → Normal (reached, emits the uint handle slots)

    // Generate the shader source and the matching .vmat field list from the one walk, so the
    // packed values land at the cooker-reflected offsets by construction.
    const Result<GeneratedFragment> generated =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(generated.has_value());

    // --- Write a self-contained pack: canonical layout, surface vertex shader, the
    // graph-sourced fragment, and the material. ---
    const path dir = std::filesystem::temp_directory_path() / "veng_gpu_graph_layout";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    WriteFile(dir / "canonical.vlayout.json", R"({
  "elements": [
    { "format": "RGB32Sfloat",  "name": "a_Position" },
    { "format": "RGB32Sfloat",  "name": "a_Normal" },
    { "format": "RGBA32Sfloat", "name": "a_Tangent" },
    { "format": "RG32Sfloat",   "name": "a_UV" }
  ]
})");

    // The canonical-layout surface vertex stage (matches the core surface.vert contract).
    WriteFile(dir / "surface.vert.slang", R"(#include "Veng/material.slang"
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1;
};
[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    float3 c0 = draw.NormalColumn0.xyz;
    float3 c1 = draw.NormalColumn1.xyz;
    float3 c2 = draw.NormalColumn2.xyz;
    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));
    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = input.a_UV;
    output.v_WorldNormal = c0 * input.a_Normal.x + c1 * input.a_Normal.y + c2 * input.a_Normal.z;
    output.v_WorldTangent = float4(c0, 1.0);
    output.v_MaterialIndex = draw.MaterialIndex;
    return output;
}
)");
    WriteFile(dir / "surface.vert.shader.json",
              R"({ "source": "surface.vert.slang", "entry": "vsMain", "vertex_layout": 9701 })");

    WriteFile(dir / "tint.frag.graph.json", WriteNodeGraph(graph, catalog));
    WriteFile(dir / "tint.frag.shader.json",
              R"({ "source": "tint.frag.graph.json", "entry": "fsMain", "domain": "surface" })");

    // The .vmat field list comes from the same walk as the shader, so its packed values agree
    // with the reflected offsets by construction.
    const MaterialShaderInterface iface{
        .Fields = {}, .VertexShader = AssetId{9702}, .FragmentShader = AssetId{9703}};
    WriteFile(dir / "tint.vmat.json",
              WriteMaterialVmat(generated->Fields, iface, MaterialDomain::Surface));

    WriteFile(dir / "pack.json", R"({
  "version": 1,
  "assets": [
    { "id": 9701, "type": "vertex_layout", "source": "canonical.vlayout.json" },
    { "id": 9702, "type": "shader",        "source": "surface.vert.shader.json" },
    { "id": 9703, "type": "shader",        "source": "tint.frag.shader.json" },
    { "id": 9704, "type": "material",      "source": "tint.vmat.json" }
  ]
})");

    // --- Cook + mount. ---
    const path outArchive = dir / "out.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cooked = cooker.CookPack(dir / "pack.json", outArchive, {}, nullptr, nullptr,
                                              nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{9704});
    REQUIRE(material.has_value());
    REQUIRE(material->IsLoaded());

    // --- Render the brick cube and read the albedo g-buffer at the center. ---
    constexpr uvec2 extent{128, 128};
    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *material), "Graph Layout Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Terminate after the g-buffer (DebugView::Albedo): the albedo channel is the
    // material's authored param color directly, no lighting in between.
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Albedo, .Bloom = false, .Shadows = false, .AO = false},
    });

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            renderer->Execute(
                cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
        });
    const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);

    // The center samples the cube face: the albedo must be the authored param color. A
    // mis-ordered struct would read the vec4 from the wrong offset (the uint handle bits or
    // a pad), so the center would not match — the regression this guards.
    const vec3 center = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(center.r == doctest::Approx(ParamAlbedo.x).epsilon(0.05));
    CHECK(center.g == doctest::Approx(ParamAlbedo.y).epsilon(0.05));
    CHECK(center.b == doctest::Approx(ParamAlbedo.z).epsilon(0.05));
    // Blue-dominant, definitively not black — the read landed at the right offset.
    CHECK(center.b > center.r);
    CHECK(center.r > 0.05f);

    std::filesystem::remove_all(dir);
}
