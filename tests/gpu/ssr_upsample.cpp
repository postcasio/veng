// Screen-space reflections magnified past the SSR resolution stay smooth and stop at silhouettes.
// Each renderer here has a render allocation of half its output and traces at Half SSR, so the
// trace runs at a quarter of the output per axis and one SSR texel spans two composite pixels.
// Two properties:
//
//  - a glossy floor's reflection of a lit sphere carries no structure at the SSR texel period:
//    along every row and column of floor, how far each pixel's second difference departs from its
//    neighbours' mean (the fourth difference, where a bilinear magnification leaves a step once per
//    texel) stays small against the reflection's peak, and holds steady, at every quarter-texel
//    camera offset across a whole texel;
//  - a reflection stops at a silhouette: a polished panel standing on a polished floor, whose own
//    rays reflect nothing, takes none of the floor's reflection beside its edges, and the floor
//    keeps its whole reflection up to them rather than averaging in the panel's empty one.
//
// Every measure is on the linear HDR target the SSR composite writes, as the difference between a
// renderer with SSR and one without, so each surface's own shading cancels. The polished surfaces
// are white metal, whose Fresnel term is one: the difference is the reflection itself. Skips
// cleanly (exit 77) on a machine with no Vulkan ICD.

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

    // The output, and the render allocation the scene and the SSR composite run at.
    constexpr uvec2 Extent{256, 256};
    constexpr uvec2 RenderExtent{128, 128};

    // The polished surfaces' roughness: glossy enough that the composite reads level 0 alone.
    constexpr f32 GlossyRoughness = 0.05f;

    // What a composite pixel's g-buffer normal says it is.
    enum class Surface : u8
    {
        Other,
        Floor,
        Upright,
    };

    vec3 DecodeTexel(const vector<u8>& rgba16f, const usize index)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        return vec3(glm::unpackHalf1x16(halves[index * 4 + 0]),
                    glm::unpackHalf1x16(halves[index * 4 + 1]),
                    glm::unpackHalf1x16(halves[index * 4 + 2]));
    }

    f32 Luma(const vec3& c)
    {
        return glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
    }

    path MountPack(AssetManager& assets, const char* archiveName)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "forward_lighting_pack.json",
                                              outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                                              {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        return outArchive;
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

    // A glossy white metal floor at y = 0, lit by a directional light from behind the camera, which
    // stands 0.9 m up looking down -Z.
    struct GlossyScene
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        CameraView Camera;
    };

    void Place(GlossyScene& built, const AssetManager& assets, const Ref<Mesh>& mesh,
               const vec3& position, const quat& rotation)
    {
        built.Meshes.push_back(mesh);
        const Entity entity = built.World->CreateEntity();
        auto& transform = built.World->Add<Transform>(entity);
        transform.Position = position;
        transform.Rotation = rotation;
        built.World->Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
    }

    // Stands the camera `offset` along X from its home, looking the same way.
    void AimCamera(GlossyScene& built, const f32 offset)
    {
        built.Camera.SetView(vec3(offset, 0.9f, 2.2f), vec3(offset, 0.1f, -1.0f),
                             vec3(0.0f, 1.0f, 0.0f));
    }

    GlossyScene BuildFloor(Context& context, AssetManager& assets, TypeRegistry& types)
    {
        GlossyScene built{.World = Scene::Create(types)};
        Place(built, assets,
              Mesh::BuildSync(context,
                              Primitives::Plane(vec2(40.0f), uvec2(1),
                                                MakeMaterial(context, assets, "Floor", vec3(1.0f),
                                                             GlossyRoughness, 1.0f)),
                              "Glossy Floor"),
              vec3(0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));

        const Entity light = built.World->CreateEntity();
        built.World->Add<Light>(light) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.0f, -0.4f, -1.0f)),
            .Color = vec3(1.0f),
            .Intensity = 8.0f / LuminousAnchor,
        };

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
        AimCamera(built, 0.0f);
        return built;
    }

    // A plane `size` across standing upright at `position`, facing the camera.
    void PlaceUpright(GlossyScene& built, Context& context, AssetManager& assets, const char* name,
                      const vec2& size, const vec3& position, const vec3& colour, f32 roughness,
                      f32 metallic)
    {
        Place(built, assets,
              Mesh::BuildSync(context,
                              Primitives::Plane(
                                  size, uvec2(1),
                                  MakeMaterial(context, assets, name, colour, roughness, metallic)),
                              name),
              position, glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f)));
    }

    // One frame's reflection: the luminance SSR adds to each composite pixel, and what each pixel is.
    struct Reflection
    {
        vector<f32> Added;
        vector<Surface> Surfaces;

        [[nodiscard]] f32 AddedAt(const u32 x, const u32 y) const
        {
            return Added[static_cast<usize>(y) * RenderExtent.x + x];
        }

        [[nodiscard]] Surface SurfaceAt(const u32 x, const u32 y) const
        {
            return Surfaces[static_cast<usize>(y) * RenderExtent.x + x];
        }
    };

    // Two renderers over one scene at half render scale and Half SSR, one with SSR and one without,
    // and a third drawing the normals at their render resolution, which tells each composite pixel's
    // surface (the g-buffer itself carries no transfer usage).
    struct ReflectionProbe
    {
        Unique<SceneRenderer> Traced;
        Unique<SceneRenderer> Plain;
        Unique<SceneRenderer> Normals;

        ReflectionProbe(Context& context, AssetManager& assets)
        {
            SceneRendererSettings settings{.Mode = DebugView::Final,
                                           .Bloom = false,
                                           .Shadows = false,
                                           .SSR = true,
                                           .AO = false};
            settings.SsrResolutionScale = SceneRendererSettings::SsrResolution::Half;
            Traced = SceneRenderer::Create({.Context = context,
                                            .Assets = assets,
                                            .OutputFormat = context.GetOutputFormat(),
                                            .Extent = Extent,
                                            .RenderExtent = RenderExtent,
                                            .Settings = settings});
            settings.SSR = false;
            Plain = SceneRenderer::Create({.Context = context,
                                           .Assets = assets,
                                           .OutputFormat = context.GetOutputFormat(),
                                           .Extent = Extent,
                                           .RenderExtent = RenderExtent,
                                           .Settings = settings});
            settings.Mode = DebugView::Normal;
            Normals = SceneRenderer::Create({.Context = context,
                                             .Assets = assets,
                                             .OutputFormat = context.GetOutputFormat(),
                                             .Extent = RenderExtent,
                                             .Settings = settings});
        }

        Reflection Render(Context& context, const GlossyScene& scene)
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
                    Normals->Execute(cmd, view);
                });
            const vector<u8> traced = Traced->GetHdrView()->GetImage()->Download();
            const vector<u8> plain = Plain->GetHdrView()->GetImage()->Download();
            const vector<u8> normals = Normals->GetOutput()->GetImage()->Download();
            const usize count = static_cast<usize>(RenderExtent.x) * RenderExtent.y;
            REQUIRE(normals.size() == count * 8);
            Reflection frame;
            frame.Added.resize(count);
            frame.Surfaces.resize(count);
            for (usize i = 0; i < count; ++i)
            {
                frame.Added[i] = Luma(DecodeTexel(traced, i)) - Luma(DecodeTexel(plain, i));
                // The debug arm encodes a normal as n * 0.5 + 0.5.
                const vec3 normal = DecodeTexel(normals, i) * 2.0f - 1.0f;
                frame.Surfaces[i] = normal.y > 0.9f   ? Surface::Floor
                                    : normal.z > 0.9f ? Surface::Upright
                                                      : Surface::Other;
            }
            return frame;
        }
    };

    // Whether every pixel within `radius` of (x, y) is floor, so the composite's taps there all
    // land on floor and no silhouette shapes the reflection.
    bool FloorAround(const Reflection& frame, const u32 x, const u32 y, const u32 radius)
    {
        for (u32 dy = 0; dy <= 2 * radius; ++dy)
        {
            for (u32 dx = 0; dx <= 2 * radius; ++dx)
            {
                if (frame.SurfaceAt(x + dx - radius, y + dy - radius) != Surface::Floor)
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr upsample: a magnified glossy reflection carries no SSR texel grid")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_upsample_grid.vengpack");
    GlossyScene built = BuildFloor(Context, assets, Types);
    const Ref<Mesh> sphere = Mesh::BuildSync(
        Context,
        Primitives::Sphere(0.25f, 32, 64,
                           MakeMaterial(Context, assets, "Ball", vec3(1.0f), 1.0f, 0.0f)),
        "Ball");
    Place(built, assets, sphere, vec3(0.0f, 0.45f, -0.6f), quat(1.0f, 0.0f, 0.0f, 0.0f));
    ReflectionProbe probe(Context, assets);

    // One SSR texel's width where the sphere's reflection appears, about 3.1 m from the camera.
    const f32 ssrTexel =
        2.0f * 3.1f * std::tan(glm::radians(30.0f)) / static_cast<f32>(RenderExtent.y / 2);
    // The taps of a pixel this far from anything but floor all land on floor.
    constexpr u32 TapReach = 3;
    constexpr u32 Offsets = 4;
    f32 structureMin = 1e30f;
    f32 structureMax = 0.0f;
    for (u32 offset = 0; offset < Offsets; ++offset)
    {
        AimCamera(built, ssrTexel * static_cast<f32>(offset) / static_cast<f32>(Offsets));
        const Reflection frame = probe.Render(Context, built);

        f32 peak = 0.0f;
        for (usize i = 0; i < frame.Added.size(); ++i)
        {
            if (frame.Surfaces[i] == Surface::Floor)
            {
                peak = std::max(peak, frame.Added[i]);
            }
        }
        REQUIRE(peak > 0.0f);

        f32 structure = 0.0f;
        for (u32 y = TapReach; y + TapReach < RenderExtent.y; ++y)
        {
            for (u32 x = TapReach; x + TapReach < RenderExtent.x; ++x)
            {
                if (!FloorAround(frame, x, y, TapReach))
                {
                    continue;
                }
                const auto Fourth = [&](const i32 dx, const i32 dy)
                {
                    const auto At = [&](const i32 k)
                    {
                        return frame.AddedAt(static_cast<u32>(static_cast<i32>(x) + k * dx),
                                             static_cast<u32>(static_cast<i32>(y) + k * dy));
                    };
                    return std::abs(At(-2) - 4.0f * At(-1) + 6.0f * At(0) - 4.0f * At(1) + At(2)) *
                           0.5f;
                };
                structure = std::max({structure, Fourth(1, 0), Fourth(0, 1)});
            }
        }
        structureMin = std::min(structureMin, structure / peak);
        structureMax = std::max(structureMax, structure / peak);
    }

    // A bilinear magnification leaves about a quarter of a reflected step's height here, by an
    // amount that moves with where the step falls on the grid; the B-spline about a tenth, wherever.
    CHECK(structureMax < 0.15f);
    CHECK((structureMax - structureMin) / structureMax < 0.05f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr upsample: a magnified reflection stops at a silhouette, from either side")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_upsample_silhouette.vengpack");
    GlossyScene built = BuildFloor(Context, assets, Types);
    // A bright matte backdrop for the floor to reflect, and in front of it a polished panel facing
    // the camera, whose reflected rays head back toward the camera and hit nothing on screen.
    PlaceUpright(built, Context, assets, "Backdrop", vec2(12.0f, 6.0f), vec3(0.0f, 3.0f, -5.0f),
                 vec3(0.9f), 1.0f, 0.0f);
    PlaceUpright(built, Context, assets, "Panel", vec2(0.6f, 0.8f), vec3(0.0f, 0.4f, -0.5f),
                 vec3(1.0f), GlossyRoughness, 1.0f);
    ReflectionProbe probe(Context, assets);
    const Reflection frame = probe.Render(Context, built);

    // The rows where floor flanks the panel on both sides.
    u32 top = RenderExtent.y;
    u32 bottom = 0;
    for (u32 y = 0; y < RenderExtent.y; ++y)
    {
        if (frame.SurfaceAt(0, y) == Surface::Floor &&
            frame.SurfaceAt(RenderExtent.x / 2, y) == Surface::Upright)
        {
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    REQUIRE(bottom > top + 12);

    // Through the panel's lower half, clear of the floor's own reflection of the panel's top
    // corners and of the crease where it meets the floor, both edges of every row.
    constexpr u32 Reach = 3;
    f32 panelMost = 0.0f;
    f32 floorLeast = 1e30f;
    u32 rows = 0;
    for (u32 y = (top + bottom) / 2; y + 3 <= bottom; ++y)
    {
        u32 left = RenderExtent.x / 2;
        while (frame.SurfaceAt(left - 1, y) == Surface::Upright)
        {
            --left;
        }
        u32 right = RenderExtent.x / 2;
        while (frame.SurfaceAt(right + 1, y) == Surface::Upright)
        {
            ++right;
        }
        REQUIRE(left > 16);
        REQUIRE(right + 16 < RenderExtent.x);
        // The floor's reflection here, well clear of the panel.
        const f32 plateau = 0.5f * (frame.AddedAt(left - 12, y) + frame.AddedAt(right + 12, y));
        REQUIRE(plateau > 0.0f);
        for (u32 k = 0; k < Reach; ++k)
        {
            panelMost = std::max({panelMost, frame.AddedAt(left + k, y) / plateau,
                                  frame.AddedAt(right - k, y) / plateau});
            floorLeast = std::min({floorLeast, frame.AddedAt(left - 1 - k, y) / plateau,
                                   frame.AddedAt(right + 1 + k, y) / plateau});
        }
        ++rows;
    }
    REQUIRE(rows >= 4);

    CHECK(panelMost < 0.02f);
    CHECK(floorLeast > 0.95f);

    std::filesystem::remove(archive);
}
