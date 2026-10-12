// Instanced mesh sets through the SceneRenderer: the per-instance cull keeps what each view sees,
// the size band partitions the instances by the reference projected size in every view, levels of
// detail follow size and hold inside the hysteresis band, the cull runs whatever the entity cull
// mode, a set stays outside the per-frame draw-candidate cap, a partial write moves exactly what it
// wrote, a set casts into the cascades, and the host fallback keeps what the GPU cull keeps.
//
// Every scene is a field of unit cubes laid on a grid in front of (and behind) the camera.

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include <doctest/doctest.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include <Renderer/SceneRendererIds.h>

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm is first
// included.
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The fixture pack's material, which declares "instancedSets" and tints by the instance datum.
    constexpr AssetId InstanceTintMaterial{0xB28AC9C2A4D595F9ULL};

    constexpr uvec2 Extent{128, 128};

    const quat Identity(1.0f, 0.0f, 0.0f, 0.0f);

    path CookAndMount(AssetManager& assets, const char* archiveName)
    {
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        // The fixture shaders include the engine's surface and instanced-set contracts.
        REQUIRE(Veng::TestSupport::CookCached(
                    cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "instanced_pack.json", outArchive, {},
                    nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        return outArchive;
    }

    CameraView MakeCamera(const vec3& eye, const vec3& target, const f32 fovDegrees,
                          const vec3& up = vec3(0.0f, 1.0f, 0.0f))
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(fovDegrees), 1.0f, 0.1f, 1000.0f);
        camera.SetView(eye, target, up);
        return camera;
    }

    Unique<SceneRenderer> MakeRenderer(Context& context, AssetManager& assets,
                                       const SceneRendererSettings& settings)
    {
        return SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = Format::RGBA16Sfloat,
            .Extent = Extent,
            .Settings = settings,
        });
    }

    // The settings of a cull-only case: the g-buffer and nothing after it.
    SceneRendererSettings AlbedoOnly()
    {
        return SceneRendererSettings{.Mode = DebugView::Albedo, .Occlusion = false};
    }

    void Render(const Context& context, SceneRenderer& renderer, const Scene& scene,
                const CameraView& camera)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(
                    cmd, Renderer::SceneView{.World = scene, .Camera = camera, .Delta = 0.0f});
            });
    }

    Entity AddSet(Scene& scene, const Ref<Mesh>& mesh,
                  const AssetHandle<MaterialInstance>& material, const Ref<InstanceSet>& store)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        auto& set = scene.Add<InstancedMesh>(entity);
        set.Mesh = AssetManager::Adopt(mesh);
        set.Material = material;
        set.Instances = store;
        return entity;
    }

    // A strip of half-unit cubes ten wide, one unit apart, running from z = -length/2 to
    // +length/2 along the floor: a camera standing above its middle sees the half ahead.
    Ref<InstanceSet> Strip(const u32 rows)
    {
        const Ref<InstanceSet> store = InstanceSet::Create(rows * 10);
        vector<MeshInstance> instances;
        instances.reserve(static_cast<usize>(rows) * 10);
        for (u32 row = 0; row < rows; ++row)
        {
            for (u32 column = 0; column < 10; ++column)
            {
                const vec3 at(static_cast<f32>(column) - 4.5f, 0.0f,
                              static_cast<f32>(row) - static_cast<f32>(rows) * 0.5f + 0.5f);
                instances.push_back(MeshInstance::Make(at, Identity, 0.5f));
            }
        }
        store->Write(0, instances);
        return store;
    }

    // The instances of one set one view kept, by index.
    std::set<u32> Kept(const vector<InstancedMeshSurvivors>& readback, const Entity set,
                       const u32 view)
    {
        std::set<u32> kept;
        for (const InstancedMeshSurvivors& entry : readback)
        {
            if (entry.Set == set && entry.View == view)
            {
                for (const InstancedMeshSurvivor& survivor : entry.Survivors)
                {
                    kept.insert(survivor.Instance);
                }
            }
        }
        return kept;
    }

    struct InstancedScene
    {
        AssetManager& Assets;
        path Archive;
        AssetHandle<MaterialInstance> Material;
        Ref<Mesh> Cube;
    };

    InstancedScene Prepare(Context& context, AssetManager& assets, const char* archive)
    {
        InstancedScene scene{.Assets = assets, .Archive = CookAndMount(assets, archive)};
        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(InstanceTintMaterial);
        REQUIRE(material.has_value());
        scene.Material = *material;
        scene.Cube = Mesh::BuildSync(context, Primitives::Cube(1.0f), "Instanced Cube");
        return scene;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: a set draws what is in view and nothing else")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_view.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<InstanceSet> store = Strip(1000);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, store);

    // A 120-degree camera above the strip's middle: everything ahead from a couple of units out is
    // in view, everything behind is not.
    const CameraView camera = MakeCamera(vec3(0.0f, 3.1f, 0.0f), vec3(0.0f, 3.1f, -1.0f), 120.0f);
    const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, AlbedoOnly());
    Render(Context, *renderer, *scene, camera);

    const std::set<u32> kept = Kept(renderer->ReadbackInstancedMeshSurvivors(), set, 0);
    CHECK(kept.size() > 4900);
    CHECK(kept.size() <= 5000);

    // Every survivor's bound meets the frustum, and none is behind the camera.
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());
    const AABB cubeBounds = fixture.Cube->GetBounds();
    u32 outside = 0;
    u32 behind = 0;
    for (const u32 i : kept)
    {
        const MeshInstance& instance = store->GetInstances()[i];
        AABB bounds = cubeBounds.Transformed(instance.GetMatrix());
        bounds.Min -= vec3(1e-3f);
        bounds.Max += vec3(1e-3f);
        outside += Intersects(frustum, bounds) ? 0u : 1u;
        behind += instance.GetPosition().z > 0.0f ? 1u : 0u;
    }
    CHECK(outside == 0);
    CHECK(behind == 0);

    const InstancedMeshStats stats = renderer->GetInstancedMeshStats();
    CHECK(stats.Sets == 1);
    CHECK(stats.Instances == 10000);
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: levels follow size, and hold inside the hysteresis band")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_lod.vengpack");

    // A cube whose level 1 draws one face, switching where its projected size falls below 0.3.
    MeshData data = Primitives::Cube(1.0f);
    data.Lods = {
        MeshLod{.ScreenSize = 0.3f, .Ranges = {MeshLodRange{.IndexOffset = 0, .IndexCount = 6}}}};
    const Ref<Mesh> leveled = Mesh::BuildSync(Context, data, "Leveled Instanced Cube");
    REQUIRE(leveled->GetLodCount() == 2);

    const CameraView camera = MakeCamera(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), 60.0f);
    const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, AlbedoOnly());

    SUBCASE("levels are non-decreasing with distance")
    {
        const Unique<Scene> scene = Scene::Create(Types);
        const Ref<InstanceSet> store = InstanceSet::Create(40);
        vector<MeshInstance> line;
        for (u32 i = 0; i < 40; ++i)
        {
            line.push_back(MeshInstance::Make(vec3(0.0f, 0.0f, -2.0f - 0.5f * static_cast<f32>(i)),
                                              Identity, 1.0f));
        }
        store->Write(0, line);
        const Entity set = AddSet(*scene, leveled, fixture.Material, store);
        Render(Context, *renderer, *scene, camera);

        vector<u32> levels(40, ~0u);
        for (const InstancedMeshSurvivors& entry : renderer->ReadbackInstancedMeshSurvivors())
        {
            if (entry.Set == set && entry.View == 0)
            {
                for (const InstancedMeshSurvivor& survivor : entry.Survivors)
                {
                    levels[survivor.Instance] = survivor.Lod;
                }
            }
        }
        CHECK(std::ranges::find(levels, ~0u) == levels.end());
        CHECK(std::ranges::is_sorted(levels));
        CHECK(levels.front() == 0);
        CHECK(levels.back() == 1);
    }

    SUBCASE("an instance moved across the switch by less than the band keeps its level")
    {
        const Unique<Scene> scene = Scene::Create(Types);
        const Ref<InstanceSet> store = InstanceSet::Create(1);
        const Entity set = AddSet(*scene, leveled, fixture.Material, store);

        // The cube's size crosses 0.3 at a distance of half-diagonal x P11 / 0.3.
        const f32 crossing = std::sqrt(3.0f) * 0.5f / std::tan(glm::radians(30.0f)) / 0.3f;
        const auto LevelAt = [&](const f32 distance)
        {
            const std::array instance{
                MeshInstance::Make(vec3(0.0f, 0.0f, -distance), Identity, 1.0f)};
            store->Write(0, instance);
            Render(Context, *renderer, *scene, camera);
            const vector<InstancedMeshSurvivors> readback =
                renderer->ReadbackInstancedMeshSurvivors();
            for (const InstancedMeshSurvivors& entry : readback)
            {
                if (entry.Set == set && entry.View == 0 && entry.Survivors.size() == 1)
                {
                    return entry.Survivors[0].Lod;
                }
            }
            return ~0u;
        };

        // The default band is +-10 %: 3 % past the switch either way stays inside it.
        CHECK(LevelAt(crossing * 0.8f) == 0);
        CHECK(LevelAt(crossing * 1.03f) == 0);
        CHECK(LevelAt(crossing * 0.97f) == 0);
        CHECK(LevelAt(crossing * 1.25f) == 1);
        CHECK(LevelAt(crossing * 0.97f) == 1);
        CHECK(LevelAt(crossing * 1.03f) == 1);
    }
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: the size band partitions, and the shadows cast the band")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_band.vengpack");

    // A 20 x 20 grid two units apart, looked down on from above and behind, all of it in view.
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<InstanceSet> store = InstanceSet::Create(400);
    vector<MeshInstance> grid;
    for (u32 i = 0; i < 400; ++i)
    {
        const vec3 at(2.0f * static_cast<f32>(i % 20) - 19.0f, 0.0f,
                      -2.0f * static_cast<f32>(i / 20) - 2.0f);
        grid.push_back(MeshInstance::Make(at, Identity, 1.0f));
    }
    store->Write(0, grid);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, store);
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{
        .Direction = glm::normalize(vec3(0.2f, -1.0f, -0.3f)),
        .Intensity = 1.0f / LuminousAnchor,
    };

    const CameraView camera = MakeCamera(vec3(0.0f, 30.0f, 20.0f), vec3(0.0f, 0.0f, -21.0f), 70.0f);

    // The band's floor at the grid's median projected size, measured the way the cull measures.
    vector<f32> pixels;
    for (const MeshInstance& instance : grid)
    {
        vec3 center;
        f32 radius = 0.0f;
        InstanceBoundingSphere(instance.GetMatrix(), fixture.Cube->GetBounds(), center, radius);
        pixels.push_back(ReferenceProjectedPixels(camera.ViewProjection(), camera.GetPosition(),
                                                  static_cast<f32>(Extent.y), center, radius));
    }
    vector<f32> sorted = pixels;
    std::ranges::sort(sorted);
    const f32 floor = sorted[sorted.size() / 2];
    scene->Get<InstancedMesh>(set).MinProjectedPixels = floor;

    SceneRendererSettings settings{.Bloom = false, .AO = false, .Occlusion = false};
    settings.MaxShadowDistance = 200.0f;
    const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, settings);
    Render(Context, *renderer, *scene, camera);
    const vector<InstancedMeshSurvivors> readback = renderer->ReadbackInstancedMeshSurvivors();

    const std::set<u32> main = Kept(readback, set, 0);
    std::set<u32> cast;
    u32 shadowViews = 0;
    for (const InstancedMeshSurvivors& entry : readback)
    {
        if (entry.Set == set && entry.View > 0)
        {
            ++shadowViews;
            for (const InstancedMeshSurvivor& survivor : entry.Survivors)
            {
                cast.insert(survivor.Instance);
            }
        }
    }
    REQUIRE(shadowViews > 0);

    // Off the threshold itself, an instance is drawn exactly when it is in the band, and every
    // instance any shadow view casts is in the band too.
    u32 misplaced = 0;
    u32 castOutsideBand = 0;
    for (u32 i = 0; i < 400; ++i)
    {
        if (std::abs(pixels[i] - floor) <= floor * 1e-3f)
        {
            continue;
        }
        const bool inBand = pixels[i] > floor;
        misplaced += (main.contains(i) != inBand) ? 1u : 0u;
        castOutsideBand += (cast.contains(i) && !inBand) ? 1u : 0u;
    }
    CHECK(misplaced == 0);
    CHECK(castOutsideBand == 0);
    // And what the main view draws, the cascades cast.
    CHECK(std::ranges::includes(cast, main));
    CHECK(main.size() > 100);
    CHECK(main.size() < 300);
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: the cull ignores the entity cull mode")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_mode.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, Strip(400));
    const CameraView camera = MakeCamera(vec3(0.0f, 3.1f, 0.0f), vec3(0.0f, 3.1f, -1.0f), 90.0f);

    const auto Survivors = [&](const SceneRendererSettings::CullMode mode)
    {
        SceneRendererSettings settings = AlbedoOnly();
        settings.Cull = mode;
        const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, settings);
        Render(Context, *renderer, *scene, camera);
        CHECK(renderer->IsInstancedMeshGpuCulled() == Context.IsGpuDrivenCullingSupported());
        return Kept(renderer->ReadbackInstancedMeshSurvivors(), set, 0);
    };
    const std::set<u32> cpu = Survivors(SceneRendererSettings::CullMode::CPU);
    CHECK_FALSE(cpu.empty());
    CHECK(cpu == Survivors(SceneRendererSettings::CullMode::GPU));
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: a set of fifty thousand stays outside the candidate cap")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_cap.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, Strip(5000));
    const Ref<Mesh> entityCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.0f, fixture.Material), "Entity Cube");
    for (u32 i = 0; i < 4; ++i)
    {
        const Entity entity = scene->CreateEntity();
        scene->Add<Transform>(entity).Position =
            vec3(static_cast<f32>(i) * 2.0f - 3.0f, 3.0f, -8.0f);
        scene->Add<MeshRenderer>(entity).Mesh = AssetManager::Adopt(entityCube);
    }

    const CameraView camera = MakeCamera(vec3(0.0f, 3.1f, 0.0f), vec3(0.0f, 3.1f, -1.0f), 120.0f);
    const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, AlbedoOnly());
    Render(Context, *renderer, *scene, camera);

    const DrawBudgetStats budget = renderer->GetDrawBudgetStats();
    CHECK(budget.StaticDropped == 0);
    CHECK(budget.InstancedSetDropped == 0);
    CHECK(budget.InstancedSets == 1);
    CHECK(budget.InstancedInstances == 50000);
    // The entities' slots and the set's one slot, however many instances the set draws.
    CHECK(budget.SlotsGranted == 5);
    CHECK(renderer->GetLastDrawnCount() == 4);
    CHECK(Kept(renderer->ReadbackInstancedMeshSurvivors(), set, 0).size() > MaxCullCandidates);
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: a partial write moves what it wrote and nothing else")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_write.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<InstanceSet> store = Strip(1000);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, store);
    const CameraView camera = MakeCamera(vec3(0.0f, 3.1f, 0.0f), vec3(0.0f, 3.1f, -1.0f), 120.0f);
    const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, AlbedoOnly());

    Render(Context, *renderer, *scene, camera);
    const std::set<u32> before = Kept(renderer->ReadbackInstancedMeshSurvivors(), set, 0);

    // Rows 100 to 199 stand ahead of the camera; send them behind it.
    vector<MeshInstance> moved(store->GetInstances().begin() + 1000,
                               store->GetInstances().begin() + 2000);
    for (MeshInstance& instance : moved)
    {
        instance.Row2.w += 600.0f;
    }
    store->Write(1000, moved);
    Render(Context, *renderer, *scene, camera);
    // A second frame reads the other frame slot's copy, which uploads the same write.
    Render(Context, *renderer, *scene, camera);
    const std::set<u32> after = Kept(renderer->ReadbackInstancedMeshSurvivors(), set, 0);

    std::set<u32> expected;
    for (const u32 i : before)
    {
        if (i < 1000 || i >= 2000)
        {
            expected.insert(i);
        }
    }
    CHECK(before.size() - expected.size() == 1000);
    CHECK(after == expected);
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "instanced mesh set: a set casts into the cascades")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_shadow.vengpack");

    // A lit floor, and a sparse grid of cubes floating above it.
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor =
        Mesh::BuildSync(Context, Primitives::Plane(vec2(40.0f), uvec2(1), fixture.Material),
                        "Instanced Shadow Floor");
    const Entity floorEntity = scene->CreateEntity();
    scene->Add<Transform>(floorEntity);
    // The floor only receives, so the one caster that changes between the renders is the set.
    auto& floorRenderer = scene->Add<MeshRenderer>(floorEntity);
    floorRenderer.Mesh = AssetManager::Adopt(floor);
    floorRenderer.CastsShadows = false;

    constexpr u32 Cubes = 16;
    const auto CubeCenter = [](const u32 i)
    {
        return vec3(4.0f * static_cast<f32>(i % 4) - 6.0f, 3.0f,
                    4.0f * static_cast<f32>(i / 4) - 6.0f);
    };
    const Ref<InstanceSet> store = InstanceSet::Create(Cubes);
    vector<MeshInstance> cubes;
    for (u32 i = 0; i < Cubes; ++i)
    {
        cubes.push_back(MeshInstance::Make(CubeCenter(i), Identity, 1.5f, vec4(1.0f)));
    }
    store->Write(0, cubes);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, store);
    // The light travels down and towards +x and +z, so each shadow falls on that side of its cube.
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{
        .Direction = glm::normalize(vec3(0.4f, -1.0f, 0.25f)),
        .Intensity = 1.0f / LuminousAnchor,
    };

    // Looking down from high above, so a cube hides little of the shadow it throws aside.
    const CameraView camera = MakeCamera(vec3(0.0f, 30.0f, 3.0f), vec3(0.0f), 50.0f);
    const SceneRendererSettings settings{.Bloom = false, .AO = false, .Occlusion = false};
    const auto RenderPixels = [&](const bool casts)
    {
        scene->Get<InstancedMesh>(set).CastsShadows = casts;
        const Unique<SceneRenderer> renderer = MakeRenderer(Context, assets, settings);
        Render(Context, *renderer, *scene, camera);
        return renderer->GetOutput()->GetImage()->Download();
    };
    const vector<u8> lit = RenderPixels(false);
    const vector<u8> shadowed = RenderPixels(true);
    REQUIRE(lit.size() == shadowed.size());

    const auto Luma = [](const vector<u8>& pixels, const usize texel)
    {
        const auto* halves = reinterpret_cast<const u16*>(pixels.data()) + texel * 4;
        return glm::unpackHalf1x16(halves[0]) + glm::unpackHalf1x16(halves[1]) +
               glm::unpackHalf1x16(halves[2]);
    };

    // Each darkened pixel, carried back to the floor, sits on the light's far side of its nearest
    // cube.
    const mat4 unproject = glm::inverse(camera.ViewProjection());
    u32 darker = 0;
    u32 brighter = 0;
    vec2 offset(0.0f);
    for (usize t = 0; t < static_cast<usize>(Extent.x) * Extent.y; ++t)
    {
        const f32 a = Luma(lit, t);
        const f32 b = Luma(shadowed, t);
        brighter += b > a + 1e-3f ? 1u : 0u;
        if (b >= a - 0.05f)
        {
            continue;
        }
        ++darker;
        const vec2 ndc =
            (vec2(static_cast<f32>(t % Extent.x), static_cast<f32>(t / Extent.x)) + 0.5f) /
                vec2(Extent) * 2.0f -
            1.0f;
        vec4 nearPoint = unproject * vec4(ndc, 1.0f, 1.0f);
        vec4 farPoint = unproject * vec4(ndc, 0.0f, 1.0f);
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        const vec3 ray = vec3(farPoint - nearPoint);
        const vec3 onFloor = vec3(nearPoint) - ray * (nearPoint.y / ray.y);
        vec2 nearest(1e9f);
        for (u32 i = 0; i < Cubes; ++i)
        {
            const vec2 fromCube =
                vec2(onFloor.x, onFloor.z) - vec2(CubeCenter(i).x, CubeCenter(i).z);
            nearest = glm::length(fromCube) < glm::length(nearest) ? fromCube : nearest;
        }
        offset += nearest;
    }
    // Sixteen shadows of cubes 1.5 units across, each partly beside its cube.
    CHECK(darker > 200);
    CHECK(brighter == 0);
    REQUIRE(darker > 0);
    offset /= static_cast<f32>(darker);
    CHECK(offset.x > 0.3f);
    CHECK(offset.y > 0.3f);
    std::filesystem::remove(fixture.Archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "instanced mesh set: the host fallback keeps what the GPU cull keeps")
{
    if (!Context.IsGpuDrivenCullingSupported())
    {
        MESSAGE(
            "GPU-driven culling unsupported on this device; there is no GPU cull to agree with");
        return;
    }
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const InstancedScene fixture = Prepare(Context, assets, "veng_gpu_instanced_host.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity set = AddSet(*scene, fixture.Cube, fixture.Material, Strip(400));
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{
        .Direction = glm::normalize(vec3(0.2f, -1.0f, -0.3f)),
        .Intensity = 1.0f / LuminousAnchor,
    };
    const CameraView camera = MakeCamera(vec3(0.0f, 3.1f, 0.0f), vec3(0.0f, 3.1f, -1.0f), 90.0f);

    const auto Survivors = [&](const bool host)
    {
        const Unique<SceneRenderer> renderer =
            MakeRenderer(Context, assets,
                         SceneRendererSettings{.Bloom = false, .AO = false, .Occlusion = false});
        renderer->SetInstancedMeshForceCpuCull(host);
        Render(Context, *renderer, *scene, camera);
        CHECK(renderer->IsInstancedMeshGpuCulled() == !host);
        std::map<u32, usize> perView;
        for (const InstancedMeshSurvivors& entry : renderer->ReadbackInstancedMeshSurvivors())
        {
            if (entry.Set == set)
            {
                perView[entry.View] = entry.Survivors.size();
            }
        }
        return perView;
    };
    const std::map<u32, usize> gpu = Survivors(false);
    CHECK(gpu.size() > 1);
    CHECK(gpu == Survivors(true));
    std::filesystem::remove(fixture.Archive);
}

#endif // GPU_GBUFFER_FIXTURE_DIR
