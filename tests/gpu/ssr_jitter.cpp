// Screen-space reflections under a temporal resolve stand where they stand without one. SSR
// composites after the resolve, which reconstructs the scene at unjittered positions, so a
// reflection traced from the jittered frame as it lies would follow the jitter from frame to frame.
// A mirror floor reflects a dark wall carrying two bright patches set checkerwise, rendered once with
// no anti-aliasing and then through a whole jitter period under TAA: every frame, the reflection's
// colour edges — the patches' vertical and horizontal borders, where depth is continuous and every
// ray hits — keep to a small fraction of a pixel of where the unjittered render puts them.
//
// A reflected silhouette is not measured: whether a ray grazing one hits depends on the depth
// buffer's rasterized coverage, which the jitter moves each frame and nothing after the resolve
// integrates. Every measure is on the linear HDR target the SSR composite writes, as the difference
// between a renderer with SSR and one without under the same anti-aliasing, so the floor's own
// shading cancels — the temporal history is the resolve's own output, ahead of the composite, so
// both renderers carry the same one. Skips cleanly (exit 77) on a machine with no Vulkan ICD.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/TaaJitter.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <algorithm>
#include <cmath>

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // forward_lighting_pack's opaque material: BaseColor, Roughness, Metallic, Occlusion, untextured.
    constexpr AssetId OpaqueMaterial{0x3AEA87F2E94072A6ULL};

    constexpr uvec2 Extent{128, 128};

    vector<f32> DecodeLuma(const vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        vector<f32> luma(static_cast<usize>(Extent.x) * Extent.y);
        for (usize i = 0; i < luma.size(); ++i)
        {
            const vec3 c(glm::unpackHalf1x16(halves[i * 4 + 0]),
                         glm::unpackHalf1x16(halves[i * 4 + 1]),
                         glm::unpackHalf1x16(halves[i * 4 + 2]));
            luma[i] = glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
        }
        return luma;
    }

    AssetHandle<MaterialInstance> MakeMaterial(Context& context, AssetManager& assets,
                                               const char* name, const vec3& colour, f32 roughness,
                                               f32 metallic)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(OpaqueMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = name, .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColor", vec4(colour, 1.0f));
        mat.SetParam("Roughness", roughness);
        mat.SetParam("Metallic", metallic);
        return instance;
    }

    // A white metal mirror floor at y = 0 and a dark matte wall 1.6 m wide and 1.2 m tall standing
    // on it 1 m down -Z, with bright patches over its lower-left and upper-right quarters 5 mm in
    // front of it — well inside the hit tolerance, so a ray landing either side of a patch's border
    // hits. Lit from behind the camera, which stands 0.9 m up looking down -Z.
    struct MirrorScene
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        CameraView Camera;
    };

    void Place(MirrorScene& built, const AssetManager& assets, const Ref<Mesh>& mesh,
               const vec3& position, const quat& rotation)
    {
        built.Meshes.push_back(mesh);
        const Entity entity = built.World->CreateEntity();
        auto& transform = built.World->Add<Transform>(entity);
        transform.Position = position;
        transform.Rotation = rotation;
        built.World->Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
    }

    MirrorScene BuildScene(Context& context, AssetManager& assets, TypeRegistry& types)
    {
        MirrorScene built{.World = Scene::Create(types)};
        Place(built, assets,
              Mesh::BuildSync(
                  context,
                  Primitives::Plane(vec2(40.0f), uvec2(1),
                                    MakeMaterial(context, assets, "Floor", vec3(1.0f), 0.0f, 1.0f)),
                  "Mirror Floor"),
              vec3(0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
        const quat upright = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
        Place(built, assets,
              Mesh::BuildSync(
                  context,
                  Primitives::Plane(vec2(1.6f, 1.2f), uvec2(1),
                                    MakeMaterial(context, assets, "Wall", vec3(0.15f), 1.0f, 0.0f)),
                  "Wall"),
              vec3(0.0f, 0.6f, -1.0f), upright);
        const AssetHandle<MaterialInstance> patch =
            MakeMaterial(context, assets, "Patch", vec3(0.9f), 1.0f, 0.0f);
        for (const vec3& centre : {vec3(-0.4f, 0.3f, -0.995f), vec3(0.4f, 0.9f, -0.995f)})
        {
            Place(built, assets,
                  Mesh::BuildSync(context, Primitives::Plane(vec2(0.8f, 0.6f), uvec2(1), patch),
                                  "Patch"),
                  centre, upright);
        }

        built.World->Add<Light>(built.World->CreateEntity()) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.0f, -0.4f, -1.0f)),
            .Color = vec3(1.0f),
            .Intensity = 8.0f / LuminousAnchor,
        };

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
        built.Camera.SetView(vec3(0.0f, 0.9f, 2.2f), vec3(0.0f, 0.1f, -1.0f),
                             vec3(0.0f, 1.0f, 0.0f));
        return built;
    }

    // Two renderers over one scene under one anti-aliasing mode, one with SSR and one without.
    struct ReflectionProbe
    {
        Unique<SceneRenderer> Traced;
        Unique<SceneRenderer> Plain;

        ReflectionProbe(Context& context, AssetManager& assets, const AntiAliasingMode mode)
        {
            SceneRendererSettings settings{.Mode = DebugView::Final,
                                           .Bloom = false,
                                           .AntiAliasing = mode,
                                           .Shadows = false,
                                           .SSR = true,
                                           .AO = false};
            Traced = SceneRenderer::Create({.Context = context,
                                            .Assets = assets,
                                            .OutputFormat = context.GetOutputFormat(),
                                            .Extent = Extent,
                                            .Settings = settings});
            settings.SSR = false;
            Plain = SceneRenderer::Create({.Context = context,
                                           .Assets = assets,
                                           .OutputFormat = context.GetOutputFormat(),
                                           .Extent = Extent,
                                           .Settings = settings});
        }

        // One frame of both renderers: the luminance the reflection adds, and the plain render's.
        std::pair<vector<f32>, vector<f32>> Render(Context& context, const MirrorScene& scene)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    const Renderer::SceneView view{.World = *scene.World,
                                                   .Camera = scene.Camera,
                                                   .Delta = 0.0f,
                                                   .SsrMaxDistance = 20.0f};
                    Traced->Execute(cmd, view);
                    Plain->Execute(cmd, view);
                });
            const vector<f32> traced = DecodeLuma(Traced->GetHdrView()->GetImage()->Download());
            vector<f32> plain = DecodeLuma(Plain->GetHdrView()->GetImage()->Download());
            vector<f32> added(traced.size());
            for (usize i = 0; i < added.size(); ++i)
            {
                added[i] = traced[i] - plain[i];
            }
            return {std::move(added), std::move(plain)};
        }
    };

    f32 At(const vector<f32>& image, const u32 x, const u32 y)
    {
        return image[static_cast<usize>(y) * Extent.x + x];
    }

    // Where, walking from `from` toward `to`, the profile first reaches `level`, interpolated.
    f32 Crossing(const vector<f32>& profile, const i32 from, const i32 to, const f32 level)
    {
        const i32 step = to > from ? 1 : -1;
        for (i32 i = from; i != to; i += step)
        {
            const f32 a = profile[static_cast<usize>(i)];
            const f32 b = profile[static_cast<usize>(i + step)];
            if (a < level && b >= level)
            {
                return static_cast<f32>(i) + static_cast<f32>(step) * (level - a) / (b - a);
            }
        }
        return static_cast<f32>(to);
    }

    // Where the profile crosses midway between its levels at `dark` and `bright`, walking from the
    // dark side.
    f32 MidCrossing(const vector<f32>& profile, const u32 dark, const u32 bright)
    {
        return Crossing(profile, static_cast<i32>(dark), static_cast<i32>(bright),
                        0.5f * (profile[dark] + profile[bright]));
    }

    // The rows and columns the colour edges are measured over, fixed from the unjittered render:
    // the vertical border across the rows reflecting the wall's lower half, and the horizontal one
    // down the columns reflecting its left half, each between a bright and a dark plateau.
    struct Bands
    {
        u32 FirstRow = 0;
        u32 LastRow = 0;
        u32 FirstColumn = 0;
        u32 LastColumn = 0;
        u32 Bright = 0;
        u32 Dark = 0;
        u32 Above = 0;
        u32 Below = 0;
    };

    // The reflected colour edges, each at its midway crossing averaged along its band.
    struct Edges
    {
        f32 Vertical = 0.0f;
        f32 Horizontal = 0.0f;
    };

    Edges Locate(const vector<f32>& added, const Bands& bands)
    {
        Edges edges;
        vector<f32> row(Extent.x);
        for (u32 y = bands.FirstRow; y <= bands.LastRow; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                row[x] = At(added, x, y);
            }
            edges.Vertical += MidCrossing(row, bands.Dark, bands.Bright);
        }
        edges.Vertical /= static_cast<f32>(bands.LastRow - bands.FirstRow + 1);

        vector<f32> column(Extent.y);
        for (u32 x = bands.FirstColumn; x <= bands.LastColumn; ++x)
        {
            for (u32 y = 0; y < Extent.y; ++y)
            {
                column[y] = At(added, x, y);
            }
            edges.Horizontal += MidCrossing(column, bands.Below, bands.Above);
        }
        edges.Horizontal /= static_cast<f32>(bands.LastColumn - bands.FirstColumn + 1);
        return edges;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr jitter: a reflection under TAA stands where it stands with no anti-aliasing")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_ssr_jitter.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "forward_lighting_pack.json",
                                          archive, {}, nullptr, nullptr, nullptr, nullptr, {},
                                          path(VENG_CORE_SHADER_DIR))
                .has_value());
    REQUIRE(assets.Mount(archive).has_value());
    const MirrorScene built = BuildScene(Context, assets, Types);

    ReflectionProbe still(Context, assets, AntiAliasingMode::None);
    const auto [stillAdded, stillPlain] = still.Render(Context, built);

    // Left of centre, the column reflects the lower-left patch near the contact row
    // and the dark wall above it further down; a quarter of the way down the patch's reflection,
    // the row is bright left of the border and dark right of it.
    const u32 left = Extent.x / 2 - 14;
    vector<f32> column(Extent.y);
    u32 contact = 0;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        column[y] = At(stillAdded, left, y);
        contact = At(stillPlain, left, y) > At(stillPlain, left, contact) ? y : contact;
    }
    // The patch's lowest row, walking down from its brightest.
    const f32 patchLevel = 0.5f * At(stillPlain, left, contact);
    while (contact + 1 < Extent.y && At(stillPlain, left, contact + 1) > patchLevel)
    {
        ++contact;
    }
    const f32 patchPeak = *std::max_element(column.begin() + contact + 1, column.end());
    REQUIRE(patchPeak > 0.0f);
    u32 border = contact + 1;
    while (border + 1 < Extent.y && column[border + 1] > 0.5f * patchPeak)
    {
        ++border;
    }
    REQUIRE(border > contact + 16);
    REQUIRE(border + 12 < Extent.y);

    Bands bands;
    bands.FirstRow = contact + (border - contact) / 4;
    bands.LastRow = border - (border - contact) / 4;
    bands.Bright = Extent.x / 2 - 12;
    bands.Dark = Extent.x / 2 + 12;
    bands.FirstColumn = left - 6;
    bands.LastColumn = left + 6;
    bands.Above = border - 6;
    bands.Below = border + 6;
    // Both borders separate a bright plateau from a darker one.
    REQUIRE(At(stillAdded, bands.Bright, bands.FirstRow) >
            2.0f * At(stillAdded, bands.Dark, bands.FirstRow));
    REQUIRE(At(stillAdded, left, bands.Above) > 2.0f * At(stillAdded, left, bands.Below));
    const Edges reference = Locate(stillAdded, bands);

    // One period settles the history; the next is measured, so every jitter phase is a frame.
    ReflectionProbe temporal(Context, assets, AntiAliasingMode::TAA);
    for (u32 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        temporal.Render(Context, built);
    }
    f32 verticalWorst = 0.0f;
    f32 horizontalWorst = 0.0f;
    for (u32 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        const Edges edges = Locate(temporal.Render(Context, built).first, bands);
        verticalWorst = std::max(verticalWorst, std::abs(edges.Vertical - reference.Vertical));
        horizontalWorst =
            std::max(horizontalWorst, std::abs(edges.Horizontal - reference.Horizontal));
    }
    CAPTURE(verticalWorst);
    CAPTURE(horizontalWorst);
    // Traced from the jittered frame as it lies, the mirror's horizontal border moves by most of a
    // pixel between frames; folded onto the unjittered ray, both borders keep to the resolve's own
    // ripple.
    CHECK(verticalWorst < 0.25f);
    CHECK(horizontalWorst < 0.25f);

    std::filesystem::remove(archive);
}
