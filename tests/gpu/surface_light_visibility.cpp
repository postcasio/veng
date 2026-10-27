// A Surface fragment asks the shadow maps how much of a light reaches its pixel
// (Veng/light_visibility.slang), and the g-buffer pass binds the renderer's shadow set for it.
//
// The fixture (shadow_visibility.frag) writes that visibility as its emissive with no albedo, in
// light channel 1 alone, so the light on channel 0 adds nothing and the lit target holds the
// visibility itself. A plane under a box caster then reads dark exactly where its default-lit
// twin, shaded by the deferred pass, reads in shadow, and lit where it reads lit. The same
// fragment draws through the skinned and the instanced-set stages too, each pipeline built against
// the shadow layout, and reads its occluded and open surfaces as such.

#include <algorithm>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/InstancedMesh.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace Veng;
using namespace Veng::Renderer;

#ifdef GPU_GBUFFER_FIXTURE_DIR

namespace
{
    constexpr uvec2 Extent{128, 128};
    constexpr AssetId VisibilityId{0xF9B6DEDD10C45A68ULL};
    constexpr AssetId DefaultLitId{0xF34FF2CE488F7B19ULL};
    constexpr AssetId SkinnedMeshId{0x0000000000002D13ULL};
    constexpr u32 LightTag = 1;

    path CookPack(const char* archiveName)
    {
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked = Veng::TestSupport::CookCached(
            cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "surface_lighting_pack.json", outArchive, {},
            nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cooked.has_value());
        return outArchive;
    }

    // One channel of a downloaded RGBA16F image.
    f32 Channel(const vector<u8>& rgba16f, const u32 x, const u32 y, const u32 channel)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        return glm::unpackHalf1x16(halves[(static_cast<usize>(y) * Extent.x + x) * 4 + channel]);
    }

    f32 Luma(const vector<u8>& rgba16f, const u32 x, const u32 y)
    {
        return 0.2126f * Channel(rgba16f, x, y, 0) + 0.7152f * Channel(rgba16f, x, y, 1) +
               0.0722f * Channel(rgba16f, x, y, 2);
    }

    // The lit target after one frame: lighting plus emissive, before the tone curve.
    vector<u8> RenderLit(const Context& context, SceneRenderer& renderer, const Scene& scene,
                         const CameraView& camera)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(
                    cmd, Renderer::SceneView{.World = scene, .Camera = camera, .Delta = 0.0f});
            });
        return renderer.GetHdrView()->GetImage()->Download();
    }

    // The pixel a world point projects to.
    uvec2 PixelOf(const CameraView& camera, const vec3& world)
    {
        const vec4 clip = camera.ViewProjection() * vec4(world, 1.0f);
        const vec2 uv = vec2(clip) / clip.w * 0.5f + 0.5f;
        return uvec2(glm::clamp(uv * vec2(Extent), vec2(0.0f), vec2(Extent) - 1.0f));
    }

    CameraView TopDownCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 8.0f, 0.0f), vec3(0.0f), vec3(0.0f, 0.0f, -1.0f));
        return camera;
    }

    Unique<SceneRenderer> CreateRenderer(Context& context, AssetManager& assets)
    {
        return SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = true, .AO = false},
        });
    }

    Entity AddMesh(Scene& scene, const Ref<Mesh>& mesh, const vec3& at)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity).Position = at;
        scene.Add<MeshRenderer>(entity).Mesh = AssetManager::Adopt(mesh);
        return entity;
    }

    void AddSun(Scene& scene, const vec3& direction)
    {
        scene.Add<Light>(scene.CreateEntity()) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(direction),
            .Color = vec3(1.0f),
            .Intensity = 1.0f / LuminousAnchor,
            .Tag = LightTag,
        };
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "surface light visibility: a Surface fragment and the deferred pass agree on a "
                  "shadow")
{
    RegisterBuiltinTypes(Types);
    const path outArchive = CookPack("veng_gpu_surface_light_visibility.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> visibility =
        assets.LoadSync<MaterialInstance>(VisibilityId);
    REQUIRE(visibility.has_value());
    CHECK(visibility->Get()->GetParent()->ReadsShadowMaps());
    CHECK(visibility->Get()->GetParent()->GetPipeline() == nullptr);
    const AssetResult<AssetHandle<MaterialInstance>> defaultLit =
        assets.LoadSync<MaterialInstance>(DefaultLitId);
    REQUIRE(defaultLit.has_value());
    CHECK_FALSE(defaultLit->Get()->GetParent()->ReadsShadowMaps());

    const Ref<MaterialInstance> marker = visibility->Get()->Clone("Caster Marker");
    marker->SetParam("Select", vec4(static_cast<f32>(LightTag), 1.0f, 0.0f, 0.0f));
    const AssetHandle<MaterialInstance> markerHandle = AssetManager::Adopt(marker);

    const Ref<Mesh> readingPlane =
        Mesh::BuildSync(Context, Primitives::Plane(vec2(8.0f), uvec2(1), *visibility), "Reading");
    const Ref<Mesh> twinPlane =
        Mesh::BuildSync(Context, Primitives::Plane(vec2(8.0f), uvec2(1), *defaultLit), "Twin");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity plane = AddMesh(*scene, readingPlane, vec3(0.0f));
    AddMesh(*scene, Mesh::BuildSync(Context, Primitives::Cube(1.2f, markerHandle), "Caster"),
            vec3(0.0f, 1.6f, 0.0f));
    AddSun(*scene, vec3(0.3f, -1.0f, 0.2f));

    // Looking down over the plane at the side of the caster its shadow falls on.
    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 5.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const Unique<SceneRenderer> renderer = CreateRenderer(Context, assets);
    const vector<u8> read = RenderLit(Context, *renderer, *scene, camera);
    scene->Get<MeshRenderer>(plane).Mesh = AssetManager::Adopt(twinPlane);
    const vector<u8> twin = RenderLit(Context, *renderer, *scene, camera);
    const vector<u8> normals = renderer->GetNormalView()->GetImage()->Download();

    // 0 umbra, 1 lit, 2 edge, 3 neither the plane nor readable (the caster, the background).
    const auto Class = [&](const u32 x, const u32 y) -> u32
    {
        const f32 v = Channel(read, x, y, 0);
        if (v > 1.5f || Channel(normals, x, y, 1) < 0.99f)
        {
            return 3u;
        }
        return v < 0.05f ? 0u : (v > 0.95f ? 1u : 2u);
    };

    // The interior of each region, a one-texel band about every edge excluded.
    const auto Interior = [&](const u32 x, const u32 y, const u32 cls)
    {
        for (i32 dy = -1; dy <= 1; ++dy)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                if (Class(x + dx, y + dy) != cls)
                {
                    return false;
                }
            }
        }
        return true;
    };
    vector<f32> twinUmbra;
    vector<f32> twinLit;
    for (u32 y = 1; y + 1 < Extent.y; ++y)
    {
        for (u32 x = 1; x + 1 < Extent.x; ++x)
        {
            const u32 cls = Class(x, y);
            if (cls < 2u && Interior(x, y, cls))
            {
                (cls == 0u ? twinUmbra : twinLit).push_back(Luma(twin, x, y));
            }
        }
    }
    REQUIRE(twinUmbra.size() > 200);
    REQUIRE(twinLit.size() > 2000);

    // The twin reads dark where the fragment reads its light occluded and lit where it reads it
    // open. The split between them is the midpoint of the two regions' medians, which the lit
    // plane's specular sheen (it only brightens) does not move.
    const auto Median = [](vector<f32> values)
    {
        std::ranges::nth_element(values, values.begin() + static_cast<i64>(values.size() / 2));
        return values[values.size() / 2];
    };
    const f32 umbraLevel = Median(twinUmbra);
    const f32 litLevel = Median(twinLit);
    CHECK(umbraLevel < 0.5f * litLevel);
    const f32 split = 0.5f * (umbraLevel + litLevel);
    u32 disagreements = 0;
    for (const f32 value : twinUmbra)
    {
        disagreements += value > split ? 1u : 0u;
    }
    for (const f32 value : twinLit)
    {
        disagreements += value < split ? 1u : 0u;
    }
    CHECK(disagreements == 0);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "surface light visibility: skinned and instanced-set draws read the shadow maps")
{
    RegisterBuiltinTypes(Types);
    const path outArchive = CookPack("veng_gpu_surface_light_visibility_stages.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> visibility =
        assets.LoadSync<MaterialInstance>(VisibilityId);
    REQUIRE(visibility.has_value());
    const Ref<MaterialInstance> marker = visibility->Get()->Clone("Occluder Marker");
    marker->SetParam("Select", vec4(static_cast<f32>(LightTag), 1.0f, 0.0f, 0.0f));

    const Unique<Scene> scene = Scene::Create(Types);
    // A small occluder high to the left, whose shadow along the slanted light falls about x = 0.
    AddMesh(
        *scene,
        Mesh::BuildSync(Context, Primitives::Cube(0.6f, AssetManager::Adopt(marker)), "Occluder"),
        vec3(-1.5f, 2.0f, 0.0f));
    AddSun(*scene, vec3(1.0f, -1.0f, 0.0f));

    // Two instanced cubes: one in the occluder's shadow, one in the open.
    const quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const vec3 shadowedTop(0.0f, 0.5f, 0.0f);
    const vec3 openTop(1.8f, 0.5f, 0.0f);
    const Ref<InstanceSet> store = InstanceSet::Create(2);
    store->Write(0, vector<MeshInstance>{
                        MeshInstance::Make(shadowedTop - vec3(0.0f, 0.25f, 0.0f), identity, 1.0f),
                        MeshInstance::Make(openTop - vec3(0.0f, 0.25f, 0.0f), identity, 1.0f)});
    const Entity set = scene->CreateEntity();
    scene->Add<Transform>(set);
    auto& instanced = scene->Add<InstancedMesh>(set);
    instanced.Mesh = AssetManager::Adopt(
        Mesh::BuildSync(Context, Primitives::Cube(0.5f, *visibility), "Instanced Cube"));
    instanced.Material = *visibility;
    instanced.Instances = store;

    // The skinned triangle laid flat facing up, in the open, drawn through the same material.
    const AssetResult<AssetHandle<Mesh>> skinnedMesh = assets.LoadSync<Mesh>(SkinnedMeshId);
    REQUIRE(skinnedMesh.has_value());
    REQUIRE((*skinnedMesh)->IsSkinned());
    const Entity skinned = scene->CreateEntity();
    const vec3 skinnedAt(-2.5f, 0.1f, 2.5f);
    auto& skinnedTransform = scene->Add<Transform>(skinned);
    skinnedTransform.Position = skinnedAt;
    skinnedTransform.Rotation = glm::angleAxis(glm::radians(-90.0f), vec3(1.0f, 0.0f, 0.0f));
    auto& skinnedRenderer = scene->Add<MeshRenderer>(skinned);
    skinnedRenderer.Mesh = *skinnedMesh;
    skinnedRenderer.InstanceMaterials = {*visibility};
    vector<mat4> bindPose;
    (*skinnedMesh)->GetSkeleton().Get()->ComputeBindPoseMatrices(bindPose);
    scene->Add<SkinnedPose>(skinned).Skinning = bindPose;

    const CameraView camera = TopDownCamera();
    const Unique<SceneRenderer> renderer = CreateRenderer(Context, assets);
    const vector<u8> read = RenderLit(Context, *renderer, *scene, camera);
    const vector<u8> normals = renderer->GetNormalView()->GetImage()->Download();

    const uvec2 shadowedPixel = PixelOf(camera, shadowedTop);
    const uvec2 openPixel = PixelOf(camera, openTop);
    // The triangle's centroid, (1/3, 1/3) in its own plane, carried by the entity's transform.
    const uvec2 skinnedPixel = PixelOf(camera, skinnedAt + vec3(1.0f / 3.0f, 0.0f, -1.0f / 3.0f));

    // Each probe lands on an upward-facing surface, not the background.
    CHECK(Channel(normals, shadowedPixel.x, shadowedPixel.y, 1) > 0.9f);
    CHECK(Channel(normals, openPixel.x, openPixel.y, 1) > 0.9f);
    CHECK(Channel(normals, skinnedPixel.x, skinnedPixel.y, 1) > 0.9f);

    CHECK(Channel(read, shadowedPixel.x, shadowedPixel.y, 0) < 0.05f);
    CHECK(Channel(read, openPixel.x, openPixel.y, 0) > 0.95f);
    CHECK(Channel(read, skinnedPixel.x, skinnedPixel.y, 0) > 0.95f);

    std::filesystem::remove(outArchive);
}

#endif
