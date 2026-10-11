// The stochastic reflection chain's intersection: each listed ray marched through the nearest-depth
// pyramid, its hit validated, and the lit scene there written premultiplied by the hit's confidence.
// Every case renders one frame with anti-aliasing off and reads the chain's radiance target back:
//
//  - reach is the screen: a mirror floor reflects a wall 800 m away under the default budget's
//    worth of iterations, with no reach knob;
//  - a small square 300 m away is reflected at full confidence on every texel whose mirror ray
//    meets it, so neither the end of a ray's screen line nor the thickness test loses a far hit;
//  - a thin board is not reflected into the floor behind it, whose rays pass behind it, and is
//    into the floor in front of it;
//  - a ray that spends its iteration budget is a miss;
//  - a mirror's confidence fades to nothing across the screen-edge vignette and is whole inside it;
//  - a surface seen from behind is never reflected.
//
// The materials are forward_opaque's untextured parameter block, so a white metal floor reflects
// exactly the colour it is shown. Skips cleanly (exit 77) on a machine with no Vulkan ICD.

#include <algorithm>
#include <cmath>
#include <limits>

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

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // forward_lighting_pack's opaque material: BaseColor, Roughness, Metallic, Occlusion, untextured.
    constexpr AssetId OpaqueMaterial{0x3AEA87F2E94072A6ULL};
    // white_plane_pack: the normal-mapped metal (its NormalStrength 0 makes it a flat mirror) and
    // the brick material, the parent of an unlit emitter.
    constexpr AssetId MirrorInstanceId{0xBCAE43829219AE51ULL};
    constexpr AssetId EmitterMaterial{0x000000000000232BULL};

    constexpr uvec2 Extent{128, 128};

    // A perceptual roughness whose GGX α (its square) stores as zero in the R8 α target: a mirror.
    constexpr f32 MirrorRoughness = 0.005f;

    // The iteration budget the intersection's design calls for.
    constexpr u32 TraversalBudget = 128;

    // Cooks and mounts a fixture pack, returning the archive the caller removes.
    path MountPack(AssetManager& assets, const char* archiveName,
                   const char* pack = "forward_lighting_pack.json")
    {
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, path(GPU_GBUFFER_FIXTURE_DIR) / pack,
                                              outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                                              {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        return outArchive;
    }

    AssetHandle<MaterialInstance> MakeMaterial(Context& context, AssetManager& assets,
                                               const char* name, const vec3& colour,
                                               const f32 roughness, const f32 metallic)
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

    Entity AddMesh(Scene& scene, const AssetManager& assets, const Ref<Mesh>& mesh,
                   const vec3& position, const quat& rotation = quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        const Entity entity = scene.CreateEntity();
        auto& transform = scene.Add<Transform>(entity);
        transform.Position = position;
        transform.Rotation = rotation;
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
        return entity;
    }

    // A plane standing upright, facing +Z.
    quat Upright()
    {
        return glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
    }

    void AddLight(Scene& scene)
    {
        const Entity light = scene.CreateEntity();
        scene.Add<Light>(light) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.0f, -0.3f, -1.0f)),
            .Color = vec3(1.0f),
            .Intensity = 4.0f / LuminousAnchor,
        };
    }

    SceneRendererSettings StochasticSettings()
    {
        SceneRendererSettings settings{
            .Mode = DebugView::Final, .Bloom = false, .Shadows = false, .SSR = true, .AO = false};
        settings.AntiAliasing = AntiAliasingMode::None;
        settings.SsrMethod = SceneRendererSettings::SsrTechnique::Stochastic;
        return settings;
    }

    // One frame's reflection: the radiance target, premultiplied by confidence (alpha), and the lit
    // scene it reflects.
    struct Frame
    {
        uvec2 Size;
        vector<vec4> Radiance;
        vector<vec3> Lit;

        [[nodiscard]] const vec4& At(const u32 x, const u32 y) const
        {
            return Radiance[(static_cast<usize>(y) * Size.x) + x];
        }

        [[nodiscard]] const vec3& LitAt(const u32 x, const u32 y) const
        {
            return Lit[(static_cast<usize>(y) * Size.x) + x];
        }
    };

    vector<vec4> DecodeRgba16f(const vector<u8>& bytes, const uvec2 size)
    {
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        vector<vec4> out(static_cast<usize>(size.x) * size.y);
        for (usize i = 0; i < out.size(); ++i)
        {
            out[i] = vec4(
                glm::unpackHalf1x16(halves[(i * 4) + 0]), glm::unpackHalf1x16(halves[(i * 4) + 1]),
                glm::unpackHalf1x16(halves[(i * 4) + 2]), glm::unpackHalf1x16(halves[(i * 4) + 3]));
        }
        return out;
    }

    Frame RenderOnce(const Context& context, SceneRenderer& renderer, const Scene& scene,
                     const CameraView& camera, const uvec2 size, const u32 budget = TraversalBudget)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(cmd, Renderer::SceneView{.World = scene,
                                                          .Camera = camera,
                                                          .Delta = 0.0f,
                                                          .SsrMaxSteps = budget});
            });
        const SsrTraceTargets targets = renderer.GetSsrTraceTargets();
        REQUIRE(targets.Radiance != nullptr);
        Frame frame{.Size = size,
                    .Radiance = DecodeRgba16f(targets.Radiance->GetImage()->Download(), size)};
        const vector<vec4> lit = DecodeRgba16f(renderer.GetHdrView()->GetImage()->Download(), size);
        frame.Lit.reserve(lit.size());
        for (const vec4& texel : lit)
        {
            frame.Lit.emplace_back(texel);
        }
        return frame;
    }

    f32 Luma(const vec3& c)
    {
        return glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
    }

    // The world-space ray through the centre of pixel (x, y), as the intersection reconstructs it:
    // UV is the NDC mapped to [0, 1] with no flip, and the row is UV.y times the height.
    struct PixelRay
    {
        vec3 Origin;
        vec3 Direction;
    };

    PixelRay RayThrough(const CameraView& camera, const uvec2 size, const u32 x, const u32 y)
    {
        const mat4 inverse = glm::inverse(camera.ViewProjection());
        const vec2 ndc = ((vec2(x, y) + 0.5f) / vec2(size)) * 2.0f - 1.0f;
        const vec4 nearH = inverse * vec4(ndc, 1.0f, 1.0f);
        const vec4 farH = inverse * vec4(ndc, 0.5f, 1.0f);
        const vec3 nearPoint = vec3(nearH) / nearH.w;
        const vec3 farPoint = vec3(farH) / farH.w;
        return {.Origin = nearPoint, .Direction = glm::normalize(farPoint - nearPoint)};
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr intersect: a mirror floor reflects a wall 800 m away, with no reach knob")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_intersect_far.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(4000.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Mirror", vec3(1.0f),
                                                       MirrorRoughness, 1.0f)),
                        "Mirror Floor");
    const Ref<Mesh> wall =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(2000.0f, 1200.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Far Wall",
                                                       vec3(0.9f, 0.08f, 0.04f), 1.0f, 0.0f)),
                        "Far Wall");
    AddMesh(*scene, assets, floor, vec3(0.0f));
    AddMesh(*scene, assets, wall, vec3(0.0f, 600.0f, -800.0f), Upright());
    AddLight(*scene);

    // 2 m up, looking level down -Z: the upper half of the frame is the wall, the lower half its
    // reflection.
    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 5000.0f);
    camera.SetView(vec3(0.0f, 2.0f, 0.0f), vec3(0.0f, 2.0f, -100.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(),
    });
    const Frame frame = RenderOnce(Context, *renderer, *scene, camera, Extent);

    // The wall, and the floor below the horizon where its rays meet the wall, away from the edges.
    vec3 wallSum(0.0f);
    vec4 bandSum(0.0f);
    u32 wallCount = 0;
    u32 bandCount = 0;
    for (u32 x = Extent.x * 3 / 10; x < Extent.x * 7 / 10; ++x)
    {
        for (u32 y = Extent.y * 20 / 100; y < Extent.y * 40 / 100; ++y)
        {
            wallSum += frame.LitAt(x, y);
            ++wallCount;
        }
        for (u32 y = Extent.y * 66 / 100; y < Extent.y * 84 / 100; ++y)
        {
            bandSum += frame.At(x, y);
            ++bandCount;
        }
    }
    const vec3 wallMean = wallSum / static_cast<f32>(wallCount);
    const vec4 band = bandSum / static_cast<f32>(bandCount);
    REQUIRE(wallMean.r > 0.1f);
    REQUIRE(wallMean.r > 4.0f * wallMean.g);
    // Every ray in the band reaches the wall with full confidence and carries its colour.
    CHECK(band.a > 0.95f);
    CHECK(Luma(vec3(band)) > 0.9f * Luma(wallMean));
    CHECK(band.r > 4.0f * band.g);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr intersect: a small square 300 m away is reflected at full confidence where "
                  "the mirror rays meet it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_intersect_square.vengpack", "white_plane_pack.json");
    const AssetResult<AssetHandle<MaterialInstance>> mirror =
        assets.LoadSync<MaterialInstance>(MirrorInstanceId);
    REQUIRE(mirror.has_value());
    auto& mirrorParams = const_cast<MaterialInstance&>(*mirror->Get());
    mirrorParams.SetParam("MetallicFactor", 1.0f);
    mirrorParams.SetParam("RoughnessFactor", MirrorRoughness);
    mirrorParams.SetParam("NormalStrength", 0.0f);
    const AssetResult<AssetHandle<Material>> emitterParent =
        assets.LoadSync<Material>(EmitterMaterial);
    REQUIRE(emitterParent.has_value());
    const AssetHandle<MaterialInstance> emitter = assets.BuildSync<MaterialInstance>(
        MaterialInstanceInfo{.Name = "Square", .Context = &Context, .Parent = *emitterParent});
    REQUIRE(emitter.IsLoaded());
    auto& emitterParams = const_cast<MaterialInstance&>(*emitter.Get());
    emitterParams.SetParam("BaseColorFactor", vec4(0.0f, 0.0f, 0.0f, 1.0f));
    emitterParams.SetParam("EmissiveFactor", vec4(2.0f, 2.0f, 2.0f, 0.0f));

    constexpr uvec2 Size{256, 144};
    constexpr f32 Distance = 300.0f;

    // The eye 0.3 m above a mirror, looking 10 degrees down through a 40 degree field; an emissive
    // square 2 degrees across, about seven texels, 6 degrees above the horizontal, facing the eye.
    // Its mirror image sits just ahead of the eye, and on screen each ray toward it runs into its
    // vanishing point, the square's whole depth range falling in the line's last texel.
    const vec3 eye(0.0f, 0.3f, 0.0f);
    const f32 elevation = glm::radians(6.0f);
    const vec3 sight(0.0f, std::sin(elevation), -std::cos(elevation));
    const vec3 centre = eye + sight * Distance;
    const f32 side = 2.0f * Distance * std::tan(glm::radians(1.0f));
    const quat facing = glm::angleAxis(glm::radians(90.0f) + elevation, vec3(1.0f, 0.0f, 0.0f));
    const vec3 normal = facing * vec3(0.0f, 1.0f, 0.0f);

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor =
        Mesh::BuildSync(Context, Primitives::Plane(vec2(10.0f), uvec2(1), *mirror), "Mirror Floor");
    const Ref<Mesh> square =
        Mesh::BuildSync(Context, Primitives::Plane(vec2(side), uvec2(1), emitter), "Square");
    AddMesh(*scene, assets, floor, vec3(0.0f, 0.0f, -4.5f));
    AddMesh(*scene, assets, square, centre, facing);

    CameraView camera;
    camera.SetPerspective(glm::radians(40.0f), static_cast<f32>(Size.x) / static_cast<f32>(Size.y),
                          0.05f, 2000.0f);
    const f32 pitch = glm::radians(-10.0f);
    camera.SetView(eye, eye + vec3(0.0f, std::sin(pitch), -std::cos(pitch)),
                   vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Size,
        .Settings = StochasticSettings(),
    });
    const Frame frame = RenderOnce(Context, *renderer, *scene, camera, Size);

    // Which texels' mirror rays meet the square: each pixel's ray meets the floor (y = 0, within
    // its extent), reflects, and meets the square's plane inside its edges.
    const vec3 right = glm::normalize(facing * vec3(1.0f, 0.0f, 0.0f));
    const vec3 up = glm::normalize(facing * vec3(0.0f, 0.0f, 1.0f));
    vector<bool> meets(static_cast<usize>(Size.x) * Size.y, false);
    for (u32 y = 0; y < Size.y; ++y)
    {
        for (u32 x = 0; x < Size.x; ++x)
        {
            const PixelRay ray = RayThrough(camera, Size, x, y);
            if (ray.Direction.y >= 0.0f)
            {
                continue;
            }
            const vec3 onFloor = ray.Origin + ray.Direction * (-ray.Origin.y / ray.Direction.y);
            if (std::abs(onFloor.x) > 5.0f || onFloor.z > 0.5f || onFloor.z < -9.5f)
            {
                continue;
            }
            const vec3 reflected = glm::reflect(ray.Direction, vec3(0.0f, 1.0f, 0.0f));
            const f32 toward = glm::dot(normal, reflected);
            if (std::abs(toward) < 1e-6f)
            {
                continue;
            }
            const vec3 onSquare =
                onFloor + reflected * (glm::dot(normal, centre - onFloor) / toward);
            const vec3 local = onSquare - centre;
            meets[(static_cast<usize>(y) * Size.x) + x] =
                std::abs(glm::dot(local, right)) < 0.5f * side &&
                std::abs(glm::dot(local, up)) < 0.5f * side;
        }
    }

    // The texels whose four neighbours' rays meet it too: their own rays land well inside it.
    f32 leastConfidence = 1.0f;
    f32 leastLuma = std::numeric_limits<f32>::max();
    u32 interior = 0;
    for (u32 y = 1; y + 1 < Size.y; ++y)
    {
        for (u32 x = 1; x + 1 < Size.x; ++x)
        {
            const auto Meets = [&](const u32 px, const u32 py)
            { return meets[(static_cast<usize>(py) * Size.x) + px]; };
            if (!Meets(x, y) || !Meets(x - 1, y) || !Meets(x + 1, y) || !Meets(x, y - 1) ||
                !Meets(x, y + 1))
            {
                continue;
            }
            const vec4& texel = frame.At(x, y);
            leastConfidence = std::min(leastConfidence, texel.a);
            leastLuma = std::min(leastLuma, Luma(vec3(texel)));
            ++interior;
        }
    }
    MESSAGE("interior texels " << interior << ", least confidence " << leastConfidence
                               << ", least luma " << leastLuma);
    REQUIRE(interior >= 9);
    CHECK(leastConfidence >= 0.9f);
    // The square's emission, carried whole: the radiance is the lit square at the hit.
    CHECK(leastLuma >= 0.9f * 2.0f * leastConfidence);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr intersect: a thin near board is not reflected into the floor behind it, "
                  "and is into the floor in front of it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_intersect_board.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(40.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Mirror", vec3(1.0f),
                                                       MirrorRoughness, 1.0f)),
                        "Mirror Floor");
    const Ref<Mesh> board =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(1.2f, 0.3f), uvec2(1),
                                          MakeMaterial(Context, assets, "Board",
                                                       vec3(0.9f, 0.08f, 0.04f), 1.0f, 0.0f)),
                        "Thin Board");
    AddMesh(*scene, assets, floor, vec3(0.0f));
    // A zero-thickness board floating 0.45 m to 0.75 m above the floor, 2.5 m from the camera.
    AddMesh(*scene, assets, board, vec3(0.0f, 0.6f, -0.5f), Upright());
    AddLight(*scene);

    CameraView camera;
    camera.SetPerspective(glm::radians(70.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 1.0f, 2.0f), vec3(0.0f, 0.0f, -2.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(),
    });
    // The default thickness: the floor seen beneath the board lies metres behind it.
    const Frame frame = RenderOnce(Context, *renderer, *scene, camera, Extent);

    // Down the centre column: the board is the red run of the lit scene, its mirror image the
    // first run below it whose reflection is red.
    const u32 column = Extent.x / 2;
    u32 boardTop = Extent.y;
    u32 boardBottom = 0;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        const vec3& c = frame.LitAt(column, y);
        if (c.r > 0.05f && c.r > 3.0f * c.g)
        {
            boardTop = std::min(boardTop, y);
            boardBottom = std::max(boardBottom, y);
        }
    }
    REQUIRE(boardTop < boardBottom);
    u32 imageTop = Extent.y;
    for (u32 y = boardBottom + 1; y < Extent.y; ++y)
    {
        const vec4& r = frame.At(column, y);
        if (r.r > 0.05f && r.r > 2.0f * r.g)
        {
            imageTop = y;
            break;
        }
    }
    REQUIRE(imageTop < Extent.y - 4);
    REQUIRE(imageTop > boardBottom + 8);

    // The floor through the gap reflects none of the board: its rays pass behind it. The rows next
    // to the image are left out, where a ray grazing the board's edge passes it within one texel,
    // which one depth sample per texel cannot tell from a hit. Its mirror image is the board's red.
    f32 gapRed = 0.0f;
    f32 imageRed = 0.0f;
    u32 imageCount = 0;
    for (u32 x = column - 8; x < column + 8; ++x)
    {
        for (u32 y = boardBottom + 2; y + 3 < imageTop; ++y)
        {
            gapRed = std::max(gapRed, frame.At(x, y).r);
        }
        for (u32 y = imageTop; y < std::min(imageTop + 4, Extent.y); ++y)
        {
            imageRed += frame.At(x, y).r;
            ++imageCount;
        }
    }
    CHECK(gapRed < 0.01f);
    CHECK(imageRed / static_cast<f32>(imageCount) > 0.05f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr intersect: a spent budget is a miss, and a mirror fades across the screen "
                  "edge")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_intersect_edge.vengpack");

    // A mirror floor before a wall wider than the view, the camera level: every mirror ray rises
    // straight up the screen and meets the wall in its own column.
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(200.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Mirror", vec3(1.0f),
                                                       MirrorRoughness, 1.0f)),
                        "Mirror Floor");
    const Ref<Mesh> wall = Mesh::BuildSync(
        Context,
        Primitives::Plane(vec2(100.0f, 50.0f), uvec2(1),
                          MakeMaterial(Context, assets, "Wall", vec3(0.8f), 1.0f, 0.0f)),
        "Wall");
    AddMesh(*scene, assets, floor, vec3(0.0f));
    AddMesh(*scene, assets, wall, vec3(0.0f, 25.0f, -6.0f), Upright());
    AddLight(*scene);

    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 1.0f, -10.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(),
    });

    // Floor rows from about 5 m to 2 m out, whose rays meet the wall well inside the vertical fade.
    constexpr u32 FirstRow = 90;
    constexpr u32 LastRow = 115;

    SUBCASE("a ray that spends its budget is a miss")
    {
        const Frame spent = RenderOnce(Context, *renderer, *scene, camera, Extent, 1);
        f32 most = 0.0f;
        for (const vec4& texel : spent.Radiance)
        {
            most = std::max(most, texel.a);
        }
        CHECK(most == 0.0f);
    }

    SUBCASE("the confidence fades to nothing at the screen edge and is whole inside")
    {
        const Frame frame = RenderOnce(Context, *renderer, *scene, camera, Extent);
        f32 edge = 0.0f;
        f32 inside = 1.0f;
        bool rising = true;
        for (u32 y = FirstRow; y <= LastRow; ++y)
        {
            edge = std::max({edge, frame.At(0, y).a, frame.At(Extent.x - 1, y).a});
            for (u32 x = 8; x < Extent.x - 8; ++x)
            {
                inside = std::min(inside, frame.At(x, y).a);
            }
            for (u32 x = 0; x < 8; ++x)
            {
                rising = rising && frame.At(x + 1, y).a >= frame.At(x, y).a &&
                         frame.At(Extent.x - 2 - x, y).a >= frame.At(Extent.x - 1 - x, y).a;
            }
        }
        MESSAGE("edge confidence " << edge << ", least inside " << inside);
        CHECK(edge < 0.05f);
        CHECK(inside > 0.99f);
        CHECK(rising);
    }

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr intersect: a surface seen from behind is never reflected")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_intersect_backface.vengpack");

    const Ref<Mesh> floor =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(40.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Mirror", vec3(1.0f),
                                                       MirrorRoughness, 1.0f)),
                        "Mirror Floor");
    const Ref<Mesh> panel =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(1.0f), uvec2(1),
                                          MakeMaterial(Context, assets, "Panel",
                                                       vec3(0.9f, 0.08f, 0.04f), 1.0f, 0.0f)),
                        "Panel");

    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 2.0f, 2.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(),
    });

    // The red a floor's reflection carries, summed over the frame.
    const auto ReflectedRed = [&](const quat& rotation)
    {
        const Unique<Scene> scene = Scene::Create(Types);
        AddMesh(*scene, assets, floor, vec3(0.0f));
        AddMesh(*scene, assets, panel, vec3(0.0f, 0.5f, -1.0f), rotation);
        AddLight(*scene);
        const Frame frame = RenderOnce(Context, *renderer, *scene, camera, Extent);
        f32 red = 0.0f;
        for (const vec4& texel : frame.Radiance)
        {
            red += std::max(texel.r - texel.g, 0.0f);
        }
        return red;
    };

    // A panel lying flat, facing up: the camera sees its top, but a mirror below it can only show
    // its underside, which the rays meet from behind. Stood upright facing the camera, the same
    // panel is reflected.
    const f32 flat = ReflectedRed(quat(1.0f, 0.0f, 0.0f, 0.0f));
    const f32 upright = ReflectedRed(Upright());
    MESSAGE("reflected red, flat " << flat << ", upright " << upright);
    CHECK(upright > 5.0f);
    CHECK(flat < 0.01f * upright);

    std::filesystem::remove(archive);
}
