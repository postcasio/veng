// Screen-space reflections under a temporal resolve. SSR traces the jittered frame as it lies,
// accumulates the reflection in a history of its own, reprojected by the reflected image's motion,
// and adds it to the resolved scene.
//  - A mirror floor reflects a dark wall carrying two bright patches set checkerwise, through a
//    whole jitter period under TAA: every frame, the reflection's colour edges — the patches'
//    vertical and horizontal borders, where depth is continuous and every ray hits — keep to a
//    small fraction of a pixel of where a render supersampled four times per axis puts them. Each
//    measure is the difference between a renderer with SSR and one without under the same
//    anti-aliasing, so the floor's own shading cancels.
//  - A glossy normal-mapped metal floor reflects emissive bars: through a whole jitter period, the
//    converged reflection changes by a small fraction of its level each frame. Each jitter phase
//    samples the normal map somewhere else, and so turns every reflected ray.
//  - A mirror floor reflecting the bars under a camera moving sideways shows, at each pose, close to
//    what a camera converged still there shows: its reflected image moves with the bars' reflected
//    depth, not with the floor.
//  - A still floor's converged reflection of the bars keeps almost nothing of them a frame after
//    they go, and is at their full level a frame after they return.
//
// A reflected silhouette is not measured: whether a ray grazing one hits depends on the depth
// buffer's rasterized coverage, which the jitter moves each frame. Skips cleanly (exit 77) on a
// machine with no Vulkan ICD.

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

    // The luminance of a linear RGBA16F target `scale` times Extent per axis, box-averaged down to
    // Extent.
    vector<f32> DecodeLuma(const vector<u8>& rgba16f, const u32 scale = 1)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const u32 width = Extent.x * scale;
        vector<f32> luma(static_cast<usize>(Extent.x) * Extent.y, 0.0f);
        const f32 share = 1.0f / static_cast<f32>(scale * scale);
        for (u32 y = 0; y < Extent.y * scale; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const usize i = static_cast<usize>(y) * width + x;
                const vec3 c(glm::unpackHalf1x16(halves[i * 4 + 0]),
                             glm::unpackHalf1x16(halves[i * 4 + 1]),
                             glm::unpackHalf1x16(halves[i * 4 + 2]));
                luma[static_cast<usize>(y / scale) * Extent.x + x / scale] +=
                    share * glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
            }
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

        // Renders at `scale` times Extent per axis, read back box-averaged to Extent.
        u32 Scale = 1;

        ReflectionProbe(Context& context, AssetManager& assets, const AntiAliasingMode mode,
                        const u32 scale = 1)
            : Scale(scale)
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
                                            .Extent = Extent * scale,
                                            .Settings = settings});
            settings.SSR = false;
            Plain = SceneRenderer::Create({.Context = context,
                                           .Assets = assets,
                                           .OutputFormat = context.GetOutputFormat(),
                                           .Extent = Extent * scale,
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
            const vector<f32> traced =
                DecodeLuma(Traced->GetHdrView()->GetImage()->Download(), Scale);
            vector<f32> plain = DecodeLuma(Plain->GetHdrView()->GetImage()->Download(), Scale);
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

    // The rows and columns the colour edges are measured over, fixed from the aliased render:
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

namespace
{
    // white_plane_pack's normal-mapped metal (normal_mapped.frag): its NormalMap is a field of
    // independent per-texel tilts, repeated UVScale times across the plane.
    constexpr AssetId NormalMappedInstanceId{0xBCAE43829219AE51ULL};
    // The same pack's brick material, the parent of the emissive bars and the dark wall.
    constexpr AssetId BrickMaterial{0x000000000000232BULL};

    // A glossy normal-mapped metal floor reflecting five emissive bars standing on it in front of
    // a dark wall, with no light and no ambient term: everything the floor shows is reflection.
    struct BarsScene
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        AssetHandle<MaterialInstance> Floor;
        vector<Entity> Bars;
        CameraView Camera;
    };

    AssetHandle<MaterialInstance> MakeBrick(Context& context, AssetManager& assets,
                                            const char* name, const vec3& colour,
                                            const vec3& emissive)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(BrickMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = name, .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColorFactor", vec4(colour, 1.0f));
        mat.SetParam("EmissiveFactor", vec4(emissive, 0.0f));
        return instance;
    }

    BarsScene BuildBarsScene(Context& context, AssetManager& assets, TypeRegistry& types,
                             const f32 normalStrength)
    {
        BarsScene built{.World = Scene::Create(types)};
        const AssetResult<AssetHandle<MaterialInstance>> floor =
            assets.LoadSync<MaterialInstance>(NormalMappedInstanceId);
        REQUIRE(floor.has_value());
        built.Floor = *floor;
        auto& floorMaterial = const_cast<MaterialInstance&>(*built.Floor.Get());
        floorMaterial.SetParam("MetallicFactor", 1.0f);
        floorMaterial.SetParam("RoughnessFactor", 0.05f);
        floorMaterial.SetParam("UVScale", 0.5f);
        floorMaterial.SetParam("NormalStrength", normalStrength);

        const auto place = [&](const Ref<Mesh>& mesh, const vec3& position, const quat& rotation)
        {
            built.Meshes.push_back(mesh);
            const Entity entity = built.World->CreateEntity();
            auto& transform = built.World->Add<Transform>(entity);
            transform.Position = position;
            transform.Rotation = rotation;
            built.World->Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
            return entity;
        };
        place(Mesh::BuildSync(context, Primitives::Plane(vec2(8.0f), uvec2(1), built.Floor),
                              "Normal-Mapped Floor"),
              vec3(0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
        const quat upright = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
        place(Mesh::BuildSync(context,
                              Primitives::Plane(
                                  vec2(6.0f, 2.0f), uvec2(1),
                                  MakeBrick(context, assets, "Dark Wall", vec3(0.02f), vec3(0.0f))),
                              "Dark Wall"),
              vec3(0.0f, 1.0f, -2.0f), upright);
        const AssetHandle<MaterialInstance> bar =
            MakeBrick(context, assets, "Bar", vec3(0.0f), vec3(2.0f));
        for (const f32 x : {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f})
        {
            built.Bars.push_back(
                place(Mesh::BuildSync(context, Primitives::Plane(vec2(0.12f, 0.8f), uvec2(1), bar),
                                      "Bar"),
                      vec3(x, 0.4f, -1.9f), upright));
        }

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
        built.Camera.SetView(vec3(0.0f, 0.6f, 1.5f), vec3(0.0f, 0.15f, -2.0f),
                             vec3(0.0f, 1.0f, 0.0f));
        return built;
    }

    Unique<SceneRenderer> MakeRenderer(Context& context, AssetManager& assets,
                                       const AntiAliasingMode mode, const bool ssr)
    {
        return SceneRenderer::Create({.Context = context,
                                      .Assets = assets,
                                      .OutputFormat = context.GetOutputFormat(),
                                      .Extent = Extent,
                                      .Settings = {.Mode = DebugView::Final,
                                                   .Bloom = false,
                                                   .AntiAliasing = mode,
                                                   .Shadows = false,
                                                   .PunctualShadows = false,
                                                   .SSR = ssr,
                                                   .AO = false}});
    }

    // One frame of `renderer` over `scene` from `camera`, as luminance.
    vector<f32> RenderLuma(const Context& context, SceneRenderer& renderer, const Scene& scene,
                           const CameraView& camera)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(cmd, Renderer::SceneView{.World = scene,
                                                          .Camera = camera,
                                                          .Delta = 0.0f,
                                                          .AmbientFloor = vec3(0.0f),
                                                          .SsrMaxDistance = 20.0f});
            });
        return DecodeLuma(renderer.GetHdrView()->GetImage()->Download());
    }

    path CookWhitePlane(AssetManager& assets, const char* name)
    {
        const path archive = Veng::TestSupport::TempDir() / name;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(
                    cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "white_plane_pack.json", archive, {},
                    nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(archive).has_value());
        return archive;
    }

    // The floor rows the bars' reflection falls on: below the lowest bar pixel the reflection-free
    // render shows, clear of the image border.
    struct FloorBand
    {
        u32 FirstRow = 0;
        u32 LastRow = 0;
    };

    FloorBand LocateFloor(const vector<f32>& plain)
    {
        u32 contact = 0;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                contact = At(plain, x, y) > 0.5f ? std::max(contact, y) : contact;
            }
        }
        return {.FirstRow = contact + 3, .LastRow = Extent.y - 3};
    }

    // The band's mean luminance.
    f64 BandMean(const vector<f32>& image, const FloorBand& band)
    {
        f64 sum = 0.0;
        for (u32 y = band.FirstRow; y <= band.LastRow; ++y)
        {
            for (u32 x = 2; x < Extent.x - 2; ++x)
            {
                sum += At(image, x, y);
            }
        }
        return sum / static_cast<f64>((band.LastRow - band.FirstRow + 1) * (Extent.x - 4));
    }

    // The band's mean absolute difference between two images.
    f64 BandMeanDelta(const vector<f32>& a, const vector<f32>& b, const FloorBand& band)
    {
        f64 sum = 0.0;
        for (u32 y = band.FirstRow; y <= band.LastRow; ++y)
        {
            for (u32 x = 2; x < Extent.x - 2; ++x)
            {
                sum += std::abs(At(a, x, y) - At(b, x, y));
            }
        }
        return sum / static_cast<f64>((band.LastRow - band.FirstRow + 1) * (Extent.x - 4));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr jitter: a reflection under TAA stands where a supersampled render puts it")
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
    // The edges where a render supersampled four times per axis puts them: the aliased render
    // above quantizes a hard reflected edge to its pixel grid, where SSR's temporal pass integrates
    // it across the jitter.
    ReflectionProbe supersampled(Context, assets, AntiAliasingMode::None, 4);
    const Edges reference = Locate(supersampled.Render(Context, built).first, bands);

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
    // The aliased render puts the horizontal border 0.2 px from the supersampled one; the resolve,
    // integrating the jittered traces, keeps both borders closer than that (0.13 px at landing).
    CHECK(verticalWorst < 0.25f);
    CHECK(horizontalWorst < 0.25f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr jitter: a normal-mapped mirror's reflection under TAA holds still through "
                  "a jitter period")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = CookWhitePlane(assets, "veng_gpu_ssr_jitter_normal_mapped.vengpack");
    const BarsScene built = BuildBarsScene(Context, assets, Types, 0.3f);

    const Unique<SceneRenderer> plain =
        MakeRenderer(Context, assets, AntiAliasingMode::None, false);
    const FloorBand band = LocateFloor(RenderLuma(Context, *plain, *built.World, built.Camera));
    REQUIRE(band.LastRow > band.FirstRow + 16);

    // The reflection's history runs to 64 samples where it holds still; nine periods fill it.
    const Unique<SceneRenderer> temporal =
        MakeRenderer(Context, assets, AntiAliasingMode::TAA, true);
    vector<f32> previous;
    for (u32 frame = 0; frame < 9 * TaaJitterSampleCount; ++frame)
    {
        previous = RenderLuma(Context, *temporal, *built.World, built.Camera);
    }
    const f64 reflected = BandMean(previous, band);
    REQUIRE(reflected > 0.01);
    f64 worst = 0.0;
    for (u32 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        vector<f32> current = RenderLuma(Context, *temporal, *built.World, built.Camera);
        worst = std::max(worst, BandMeanDelta(current, previous, band) / reflected);
        previous = std::move(current);
    }
    CAPTURE(reflected);
    CAPTURE(worst);
    // Each phase's reflection differs from the next by 28 % of its level; the history weights every
    // phase alike, so the average moves by about that over its count (0.3 % at landing).
    CHECK(worst < 0.01);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr jitter: a mirror's reflection under a moving camera keeps to the reflected "
                  "depth")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = CookWhitePlane(assets, "veng_gpu_ssr_jitter_strafe.vengpack");
    const BarsScene built = BuildBarsScene(Context, assets, Types, 0.0f);

    // Sideways 0.1 m a frame: the bars' reflection, at about twice the floor's depth, moves across
    // the floor by several pixels a frame.
    constexpr f32 Step = 0.1f;
    constexpr u32 MovingFrames = 6;
    const auto pose = [&](const u32 frame)
    {
        const vec3 shift(Step * static_cast<f32>(frame), 0.0f, 0.0f);
        CameraView camera = built.Camera;
        camera.SetView(vec3(0.0f, 0.6f, 1.5f) + shift, vec3(0.0f, 0.15f, -2.0f) + shift,
                       vec3(0.0f, 1.0f, 0.0f));
        return camera;
    };
    const CameraView last = pose(MovingFrames);

    // The floor shows nothing of its own: lit by nothing, it is all reflection.
    const Unique<SceneRenderer> plain =
        MakeRenderer(Context, assets, AntiAliasingMode::None, false);
    const vector<f32> unreflected = RenderLuma(Context, *plain, *built.World, last);
    const FloorBand band = LocateFloor(unreflected);
    REQUIRE(band.LastRow > band.FirstRow + 16);
    REQUIRE(BandMean(unreflected, band) < 1e-4);

    const Unique<SceneRenderer> moving = MakeRenderer(Context, assets, AntiAliasingMode::TAA, true);
    for (u32 frame = 0; frame < 2 * TaaJitterSampleCount; ++frame)
    {
        (void)RenderLuma(Context, *moving, *built.World, pose(0));
    }
    vector<f32> moved;
    for (u32 frame = 1; frame <= MovingFrames; ++frame)
    {
        moved = RenderLuma(Context, *moving, *built.World, pose(frame));
    }

    const Unique<SceneRenderer> still = MakeRenderer(Context, assets, AntiAliasingMode::TAA, true);
    vector<f32> settled;
    for (u32 frame = 0; frame < 9 * TaaJitterSampleCount; ++frame)
    {
        settled = RenderLuma(Context, *still, *built.World, last);
    }
    const f64 reflected = BandMean(settled, band);
    REQUIRE(reflected > 0.01);
    const f64 deviation = BandMeanDelta(moved, settled, band) / reflected;
    CAPTURE(reflected);
    CAPTURE(deviation);
    // The bars seen directly differ from their still-converged image by 7 to 9 % under the frame's
    // own resolve, and a reflection reprojected by the floor's motion by 40 % (7.7 % at landing).
    CHECK(deviation < 0.09);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr jitter: a converged reflection follows its source going and returning")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = CookWhitePlane(assets, "veng_gpu_ssr_jitter_reset.vengpack");

    // A mirror's history holds still with the camera; a normal-mapped floor's swings with the
    // jitter, so it is trusted to stray further before it is doubted.
    struct Case
    {
        f32 NormalStrength;
        f64 MaxResidue;
    };
    for (const Case& c : {Case{.NormalStrength = 0.0f, .MaxResidue = 0.01},
                          Case{.NormalStrength = 0.3f, .MaxResidue = 0.05}})
    {
        CAPTURE(c.NormalStrength);
        BarsScene built = BuildBarsScene(Context, assets, Types, c.NormalStrength);
        const Unique<SceneRenderer> plain =
            MakeRenderer(Context, assets, AntiAliasingMode::None, false);
        const FloorBand band = LocateFloor(RenderLuma(Context, *plain, *built.World, built.Camera));
        REQUIRE(band.LastRow > band.FirstRow + 16);

        const Unique<SceneRenderer> temporal =
            MakeRenderer(Context, assets, AntiAliasingMode::TAA, true);
        vector<f32> converged;
        for (u32 frame = 0; frame < 9 * TaaJitterSampleCount; ++frame)
        {
            converged = RenderLuma(Context, *temporal, *built.World, built.Camera);
        }
        const f64 level = BandMean(converged, band);
        REQUIRE(level > 0.01);

        // Dropped under the floor, the bars reflect nowhere: the floor now reflects a black wall.
        const auto shift = [&](const f32 dy)
        {
            for (const Entity bar : built.Bars)
            {
                built.World->Get<Transform>(bar).Position.y += dy;
            }
        };
        shift(-100.0f);
        const f64 residue =
            BandMean(RenderLuma(Context, *temporal, *built.World, built.Camera), band) / level;
        shift(100.0f);
        const f64 returned =
            BandMean(RenderLuma(Context, *temporal, *built.World, built.Camera), band) / level;
        CAPTURE(residue);
        CAPTURE(returned);
        // At landing: 0.16 % and 2.4 % left a frame after the bars went, and their full level back a
        // frame after they returned.
        CHECK(residue < c.MaxResidue);
        CHECK(returned > 0.9);
        CHECK(returned < 1.1);
    }

    std::filesystem::remove(archive);
}
