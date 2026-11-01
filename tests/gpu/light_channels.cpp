// Light channels: a light lights only the surfaces sharing a channel with it.
//
// Three surfaces — the default membership (channel 0), one that left channel 0 and joined 1, and
// one in channels 0 and 1 — under a channel-0 directional light and a channel-0|1 point light: the
// first and third take both lights, the second the point light alone. A full-coverage forward-lit
// twin of each, in the same memberships, agrees. A light's channels are a mask (naming every
// channel lights a default surface exactly as naming channel 0 does), and every combination of
// the surface flags survives the g-buffer's 8-bit channel, the translucent metering bit raising
// the code over a surface carrying every lower bit.

#include <array>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
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

#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

#ifdef GPU_GBUFFER_FIXTURE_DIR

namespace
{
    constexpr uvec2 Extent{128, 128};
    constexpr AssetId ChannelSurfaceId{0x5B182DB6DFAD64CBULL};
    constexpr AssetId ChannelForwardId{0x87CA09DD73A61DACULL};
    constexpr AssetId MeteringTranslucentId{0x7E6580614F0CE354ULL};

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

    f32 Luma(const vector<u8>& rgba16f, const uvec2 at)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(at.y) * Extent.x + at.x) * 4;
        return 0.2126f * glm::unpackHalf1x16(halves[base + 0]) +
               0.7152f * glm::unpackHalf1x16(halves[base + 1]) +
               0.0722f * glm::unpackHalf1x16(halves[base + 2]);
    }

    Unique<SceneRenderer> CreateRenderer(Context& context, AssetManager& assets,
                                         const bool autoExposure = false)
    {
        return SceneRenderer::Create({
            .Context = context,
            .Assets = assets,
            .OutputFormat = context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final,
                         .Bloom = false,
                         .Shadows = false,
                         .AO = false,
                         .AutoExposure = autoExposure},
        });
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

    AssetHandle<MaterialInstance> WithChannels(const AssetHandle<MaterialInstance>& source,
                                               const u32 channels)
    {
        const Ref<MaterialInstance> copy = source.Get()->Clone("Channel Copy");
        copy->SetParam("Channels", static_cast<f32>(channels));
        return AssetManager::Adopt(copy);
    }

    CameraView FrontCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    uvec2 PixelOf(const CameraView& camera, const vec3& world)
    {
        const vec4 clip = camera.ViewProjection() * vec4(world, 1.0f);
        return uvec2((vec2(clip) / clip.w * 0.5f + 0.5f) * vec2(Extent));
    }

    // The three surfaces' memberships and their cubes' front-face centres.
    constexpr u32 Memberships[3] = {0x1u, 0x2u, 0x3u};
    const vec3 FaceCentres[3] = {vec3(-1.3f, 0.0f, 0.5f), vec3(0.0f, 0.0f, 0.5f),
                                 vec3(1.3f, 0.0f, 0.5f)};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light channels: a light lights only the surfaces sharing a channel with it, "
                  "deferred and forward alike")
{
    RegisterBuiltinTypes(Types);
    const path outArchive = CookPack("veng_gpu_light_channels.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(ChannelSurfaceId);
    REQUIRE(opaque.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> forward =
        assets.LoadSync<MaterialInstance>(ChannelForwardId);
    REQUIRE(forward.has_value());

    const CameraView camera = FrontCamera();
    const Unique<SceneRenderer> renderer = CreateRenderer(Context, assets);

    for (const AssetHandle<MaterialInstance>& source : {*opaque, *forward})
    {
        const Unique<Scene> scene = Scene::Create(Types);
        for (u32 i = 0; i < 3; ++i)
        {
            const Entity cube = scene->CreateEntity();
            scene->Add<Transform>(cube).Position = FaceCentres[i] - vec3(0.0f, 0.0f, 0.5f);
            scene->Add<MeshRenderer>(cube).Mesh = AssetManager::Adopt(Mesh::BuildSync(
                Context, Primitives::Cube(1.0f, WithChannels(source, Memberships[i])), "Cube"));
        }
        const Entity sun = scene->CreateEntity();
        scene->Add<Light>(sun) = Light{
            .Type = LightType::Directional,
            .Direction = vec3(0.0f, 0.0f, -1.0f),
            .Color = vec3(1.0f),
            .Intensity = 0.0f,
            .Channels = 0x01,
        };
        const Entity lamp = scene->CreateEntity();
        scene->Add<Transform>(lamp).Position = vec3(0.0f, 0.0f, 2.0f);
        scene->Add<Light>(lamp) = Light{
            .Type = LightType::Point,
            .Color = vec3(1.0f),
            .Intensity = 0.0f,
            .Range = 10.0f,
            .Channels = 0x03,
        };

        // The emissive-free ambient each face reads with neither light, then each light alone.
        const auto Faces = [&](const f32 sunIntensity, const f32 lampIntensity)
        {
            scene->Get<Light>(sun).Intensity = sunIntensity;
            scene->Get<Light>(lamp).Intensity = lampIntensity;
            Render(Context, *renderer, *scene, camera);
            const vector<u8> lit = renderer->GetHdrView()->GetImage()->Download();
            std::array<f32, 3> faces{};
            for (u32 i = 0; i < 3; ++i)
            {
                faces[i] = Luma(lit, PixelOf(camera, FaceCentres[i]));
            }
            return faces;
        };
        const std::array<f32, 3> ambient = Faces(0.0f, 0.0f);
        const std::array<f32, 3> sunOnly = Faces(1.0f / LuminousAnchor, 0.0f);
        const std::array<f32, 3> lampOnly = Faces(0.0f, 16.0f * glm::pi<f32>() / LuminousAnchor);

        const auto IsLit = [&](const f32 value, const u32 i) { return value > ambient[i] + 0.05f; };
        CHECK(IsLit(sunOnly[0], 0));
        CHECK_FALSE(IsLit(sunOnly[1], 1));
        CHECK(sunOnly[1] == doctest::Approx(ambient[1]).epsilon(0.01));
        CHECK(IsLit(sunOnly[2], 2));
        CHECK(IsLit(lampOnly[0], 0));
        CHECK(IsLit(lampOnly[1], 1));
        CHECK(IsLit(lampOnly[2], 2));
    }

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light channels: naming every channel lights a default surface as channel 0 does")
{
    RegisterBuiltinTypes(Types);
    const path outArchive = CookPack("veng_gpu_light_channels_mask.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(ChannelSurfaceId);
    REQUIRE(opaque.has_value());

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity cube = scene->CreateEntity();
    scene->Add<Transform>(cube);
    scene->Add<MeshRenderer>(cube).Mesh =
        AssetManager::Adopt(Mesh::BuildSync(Context, Primitives::Cube(1.0f, *opaque), "Cube"));
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{.Type = LightType::Directional,
                                   .Direction = glm::normalize(vec3(0.3f, -0.4f, -1.0f)),
                                   .Intensity = 1.0f / LuminousAnchor};
    const Entity lamp = scene->CreateEntity();
    scene->Add<Transform>(lamp).Position = vec3(0.4f, 0.2f, 1.5f);
    scene->Add<Light>(lamp) = Light{.Type = LightType::Point,
                                    .Intensity = 4.0f * glm::pi<f32>() / LuminousAnchor,
                                    .Range = 10.0f};

    const CameraView camera = FrontCamera();
    const Unique<SceneRenderer> renderer = CreateRenderer(Context, assets);
    Render(Context, *renderer, *scene, camera);
    const vector<u8> channelZero = renderer->GetHdrView()->GetImage()->Download();
    scene->Get<Light>(sun).Channels = 0x07;
    scene->Get<Light>(lamp).Channels = 0x07;
    Render(Context, *renderer, *scene, camera);
    const vector<u8> everyChannel = renderer->GetHdrView()->GetImage()->Download();

    CHECK(Luma(channelZero, uvec2(Extent / 2u)) > 0.05f);
    CHECK(channelZero == everyChannel);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "light channels: every surface-flag combination survives the g-buffer channel")
{
    RegisterBuiltinTypes(Types);
    const path outArchive = CookPack("veng_gpu_light_channels_flags.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(ChannelSurfaceId);
    REQUIRE(opaque.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> metering =
        assets.LoadSync<MaterialInstance>(MeteringTranslucentId);
    REQUIRE(metering.has_value());

    const Ref<MaterialInstance> grid = opaque->Get()->Clone("Flag Grid");
    grid->SetParam("Mode", 1.0f);
    const Ref<MaterialInstance> lowerBits = opaque->Get()->Clone("Lower Bits");
    lowerBits->SetParam("Mode", 2.0f);
    lowerBits->SetParam("RawFlags", 63.0f / 255.0f);

    // A plane filling the view, seen from above.
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity plane = scene->CreateEntity();
    scene->Add<Transform>(plane);
    auto& planeRenderer = scene->Add<MeshRenderer>(plane);
    planeRenderer.Mesh = AssetManager::Adopt(Mesh::BuildSync(
        Context, Primitives::Plane(vec2(20.0f), uvec2(1), AssetManager::Adopt(grid)), "Plane"));
    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 5.0f, 0.0f), vec3(0.0f), vec3(0.0f, 0.0f, -1.0f));

    const Unique<SceneRenderer> renderer = CreateRenderer(Context, assets, true);
    Render(Context, *renderer, *scene, camera);
    const vector<u8> orm = renderer->GetOrmView()->GetImage()->Download();
    const auto Stored = [&](const u32 x, const u32 y)
    { return orm[(static_cast<usize>(y) * Extent.x + x) * 4 + 3]; };

    // Cell i of the 16×8 grid holds bits 0-5 of i at bits 0-5 and bit 6 at bit 7.
    constexpr uvec2 Cells{16, 8};
    u32 mismatches = 0;
    for (u32 i = 0; i < Cells.x * Cells.y; ++i)
    {
        const u32 code = (i & 0x3Fu) | ((i & 0x40u) << 1u);
        const uvec2 centre =
            uvec2(i % Cells.x, i / Cells.x) * (Extent / Cells) + Extent / (Cells * 2u);
        mismatches += Stored(centre.x, centre.y) == code ? 0u : 1u;
    }
    CHECK(mismatches == 0);

    // The metering MAX blend over a surface carrying every lower bit reads the metering bit set.
    planeRenderer.Mesh = AssetManager::Adopt(Mesh::BuildSync(
        Context, Primitives::Plane(vec2(20.0f), uvec2(1), AssetManager::Adopt(lowerBits)),
        "Plane"));
    const Entity glow = scene->CreateEntity();
    scene->Add<Transform>(glow).Position = vec3(0.0f, 1.0f, 0.0f);
    scene->Add<MeshRenderer>(glow).Mesh =
        AssetManager::Adopt(Mesh::BuildSync(Context, Primitives::Cube(1.0f, *metering), "Glow"));
    Render(Context, *renderer, *scene, camera);
    const vector<u8> blended = renderer->GetOrmView()->GetImage()->Download();
    const u8 underGlow =
        blended[(static_cast<usize>(Extent.y / 2) * Extent.x + Extent.x / 2) * 4 + 3];
    const u8 open = blended[(static_cast<usize>(4) * Extent.x + 4) * 4 + 3];
    CHECK((underGlow & 0x80u) != 0u);
    CHECK(open == 63u);

    std::filesystem::remove(outArchive);
}

#endif
