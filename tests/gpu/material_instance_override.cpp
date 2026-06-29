// Material-instance authoring → cook → render proof (GPU). Cooks a parent Surface
// material plus a *.vmatinst.json overriding the parent's exposed BaseColorFactor vec4,
// then renders a cube with the instance and a cube with the parent's zero-override
// default instance, asserting the instance shows the OVERRIDE color in the albedo
// g-buffer while the default shows the parent's authored default — the proof that the
// cooked override actually changes the rendered result. Both bind the same parent
// pipeline and own distinct SSBO slots (the parent-pipeline-shared / distinct-slot path).
//
// This is the GPU half of Plan 06's authoring loop: the *.vmatinst.json is cooked by the
// MaterialInstanceImporter (validated against the parent's reflected exposed fields) and
// loaded by the MaterialInstanceLoader, exactly as the editor inspector and the shipped
// hello-triangle sample instance do.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/packing.hpp>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The parent's authored default albedo and the instance's overriding albedo — both known,
    // distinct, non-grey colors so a correct read of each is unambiguous.
    constexpr vec4 ParentDefault{0.15f, 0.8f, 0.3f, 1.0f};  // green-dominant
    constexpr vec4 OverrideColor{0.85f, 0.2f, 0.15f, 1.0f}; // red-dominant

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

    // Renders one cube textured with the given instance, terminating after the g-buffer
    // (DebugView::Albedo), and returns the center texel's color.
    vec3 RenderCenter(Veng::Test::GpuFixture& fx, AssetManager& assets,
                      const AssetHandle<MaterialInstance>& material)
    {
        constexpr uvec2 extent{128, 128};
        const Ref<Mesh> cube =
            Mesh::BuildSync(fx.Context, Primitives::Cube(1.4f, material), "Instance Cube");

        const Unique<Scene> scene = Scene::Create(fx.Types);
        const Entity entity = scene->CreateEntity();
        scene->Add<Transform>(entity);
        scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = fx.Context,
            .Assets = assets,
            .OutputFormat = fx.Context.GetOutputFormat(),
            .Extent = extent,
            .Settings = {.Mode = DebugView::Albedo, .Bloom = false, .Shadows = false, .AO = false},
        });

        fx.Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "material instance: a cooked override renders a different color than the parent "
                  "default")
{
    RegisterBuiltinTypes(Types);

    const path dir = std::filesystem::temp_directory_path() / "veng_gpu_matinst_override";
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
    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));
    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = input.a_UV;
    output.v_WorldNormal = draw.NormalColumn0.xyz;
    output.v_WorldTangent = float4(draw.NormalColumn0.xyz, 1.0);
    output.v_MaterialIndex = draw.MaterialIndex;
    return output;
}
)");
    WriteFile(dir / "surface.vert.shader.json",
              R"({ "source": "surface.vert.slang", "entry": "vsMain", "vertex_layout": 8001 })");

    // The parent fragment writes BaseColorFactor straight to the albedo channel, so the
    // rendered albedo IS the material's exposed param — directly readable.
    WriteFile(dir / "brick.frag.slang", R"(#include "Veng/material.slang"
struct MaterialParams
{
    float4 BaseColorFactor;
};
[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams params = g_MaterialParams.Load<MaterialParams>(
        input.v_MaterialIndex * MaterialParamStride);
    GBufferOutput output;
    output.Albedo = params.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    return output;
}
)");
    WriteFile(dir / "brick.frag.shader.json",
              R"({ "source": "brick.frag.slang", "entry": "fsMain", "domain": "surface" })");

    WriteFile(dir / "brick.vmat.json",
              fmt::format(R"({{
  "domain": "surface",
  "shaders": {{ "vertex": 8002, "fragment": 8003 }},
  "fields": [
    {{ "name": "BaseColorFactor", "type": "vec4", "value": [{}, {}, {}, {}] }}
  ]
}})",
                          ParentDefault.x, ParentDefault.y, ParentDefault.z, ParentDefault.w));

    WriteFile(dir / "tinted.vmatinst.json",
              fmt::format(R"({{
  "parent": 8004,
  "overrides": {{ "BaseColorFactor": [{}, {}, {}, {}] }}
}})",
                          OverrideColor.x, OverrideColor.y, OverrideColor.z, OverrideColor.w));

    WriteFile(dir / "pack.json", R"({
  "version": 1,
  "assets": [
    { "id": 8001, "type": "vertex_layout",     "source": "canonical.vlayout.json" },
    { "id": 8002, "type": "shader",            "source": "surface.vert.shader.json" },
    { "id": 8003, "type": "shader",            "source": "brick.frag.shader.json" },
    { "id": 8004, "type": "material",          "source": "brick.vmat.json" },
    { "id": 8005, "type": "material_instance", "source": "tinted.vmatinst.json" }
  ]
})");

    const path outArchive = dir / "out.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cooked = cooker.CookPack(dir / "pack.json", outArchive, {}, nullptr, nullptr,
                                              nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // The authored instance (the override) and the bare parent's zero-override default instance.
    const AssetResult<AssetHandle<MaterialInstance>> instance =
        assets.LoadSync<MaterialInstance>(AssetId{8005});
    const string instanceErr = instance.has_value() ? string{} : instance.error().Detail;
    REQUIRE_MESSAGE(instance.has_value(), instanceErr);
    const AssetResult<AssetHandle<MaterialInstance>> defaultInstance =
        assets.LoadSync<MaterialInstance>(AssetId{8004});
    REQUIRE(defaultInstance.has_value());

    // The authored instance and the parent's default instance own distinct SSBO slots — the
    // per-instance slot the override seeds. (The pipeline-sharing invariant between two explicit
    // instances over one parent id is pinned by material_instance.cpp; the default instance here
    // builds its own private parent copy, so it is a separate Material by design.)
    CHECK(instance->Get()->GetIndex() != defaultInstance->Get()->GetIndex());
    CHECK(instance->Get()->GetPipeline().get() != nullptr);

    const vec3 overrideCenter = RenderCenter(*this, assets, *instance);
    const vec3 defaultCenter = RenderCenter(*this, assets, *defaultInstance);

    // The instance shows the override color (red-dominant); the default shows the parent
    // default (green-dominant) — proof the override actually changed the render.
    CHECK(overrideCenter.r == doctest::Approx(OverrideColor.x).epsilon(0.05));
    CHECK(overrideCenter.g == doctest::Approx(OverrideColor.y).epsilon(0.05));
    CHECK(overrideCenter.r > overrideCenter.g);

    CHECK(defaultCenter.g == doctest::Approx(ParentDefault.y).epsilon(0.05));
    CHECK(defaultCenter.g > defaultCenter.r);

    // The two cubes definitively differ.
    CHECK(std::abs(overrideCenter.r - defaultCenter.r) > 0.3f);

    std::filesystem::remove_all(dir);
}
