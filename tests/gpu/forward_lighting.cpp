// The forward light loop agrees with the deferred one. A Translucent material that includes
// Veng/forward_lighting.slang evaluates the same shared lighting core the deferred pass runs, fed
// the same per-view light state from the view block, so a translucent surface at full coverage
// renders the radiance an opaque surface with identical material parameters does. The scene lights
// one cube face with a directional and a point light over the flat ambient floor, with shadows on so
// the forward draw binds the renderer's shadow and IBL sets; the opaque cube and its translucent
// twin are rendered in turn and compared across a grid of texels on that face, where the point
// light's falloff makes each texel a different shading point.
//
// The same pair carries the environment-lighting surface flag: the opaque twin writes
// SurfaceFlagNoEnvironmentLighting into the g-buffer, the translucent twin sets
// ForwardSurface::DeclinesEnvironmentLighting, and both must take the flat ambient floor under an
// image-based or SH sky.
//
// And it carries the environment light tint: under either sky arm, a tinted face reads the tint
// times the untinted face per channel, deferred and forward, while a face declining the sky's light
// is untouched by it.

#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Environment.h>
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

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    path CookForwardPack(const char* archiveName)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult = Veng::TestSupport::CookCached(
            cooker, fixtureDir / "forward_lighting_pack.json", outArchive, {}, nullptr, nullptr,
            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());
        return outArchive;
    }

    // A pack holding one uniform equirectangular panorama under the given id, to mount from memory.
    vector<u8> PanoramaArchive(AssetId id, const vec3& colour)
    {
        constexpr u32 Width = 8;
        constexpr u32 Height = 4;
        const CookedEnvironmentHeader header{.Version = CookedEnvironmentVersion,
                                             .Format = static_cast<u32>(Format::RGBA16Sfloat),
                                             .Width = Width,
                                             .Height = Height};
        vector<u8> blob(sizeof(header) + static_cast<usize>(Width) * Height * 8);
        std::memcpy(blob.data(), &header, sizeof(header));
        auto* texels = reinterpret_cast<u16*>(blob.data() + sizeof(header));
        for (u32 i = 0; i < Width * Height; ++i)
        {
            texels[i * 4 + 0] = glm::packHalf1x16(colour.r);
            texels[i * 4 + 1] = glm::packHalf1x16(colour.g);
            texels[i * 4 + 2] = glm::packHalf1x16(colour.b);
            texels[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }
        ArchiveWriter writer;
        writer.Add(id, AssetTypes::Environment, blob);
        return writer.Build();
    }

    // Two uniform skies of very different colour and brightness, and a neutral one.
    constexpr AssetId WarmPanorama{0x62F1025508D196B5ULL};
    constexpr AssetId CoolPanorama{0x95D20444346C88C0ULL};
    constexpr AssetId NeutralPanorama{0x87CFEDC28BDDBBCAULL};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "forward lighting: a full-coverage translucent surface is lit as its opaque twin")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = CookForwardPack("veng_gpu_forward_lighting.vengpack");

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(AssetId{0xF34FF2CE488F7B19ULL});
    REQUIRE(opaque.has_value());
    REQUIRE(opaque->IsLoaded());
    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(AssetId{0xA1F58FB895475E00ULL});
    REQUIRE(translucent.has_value());
    REQUIRE(translucent->IsLoaded());
    CHECK(translucent->Get()->GetDomain() == MaterialDomain::Translucent);

    constexpr uvec2 extent{128, 128};

    const Ref<Mesh> opaqueCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *opaque), "Forward Opaque Cube");
    const Ref<Mesh> translucentCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *translucent), "Forward Lit Cube");

    const Unique<Scene> scene = Scene::Create(Types);

    // A directional light raking the front face from the upper left, and a point light close in
    // front of its right half, so the face's shading varies texel to texel.
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{
        .Type = LightType::Directional,
        .Direction = glm::normalize(vec3(0.5f, -0.4f, -1.0f)),
        .Color = vec3(1.0f, 0.95f, 0.9f),
        .Intensity = 1.5f / LuminousAnchor,
    };
    const Entity lamp = scene->CreateEntity();
    scene->Add<Transform>(lamp).Position = vec3(0.4f, 0.1f, 1.0f);
    scene->Add<Light>(lamp) = Light{
        .Type = LightType::Point,
        .Color = vec3(0.6f, 0.8f, 1.0f),
        .Intensity = 1.0f * 4.0f * glm::pi<f32>() / LuminousAnchor,
        .Range = 6.0f,
    };

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = true, .AO = false},
    });

    // The cube stands alone in the frame each time, so the translucent twin blends over nothing.
    const auto Render = [&](const Ref<Mesh>& mesh) -> vector<u8>
    {
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube).Position = vec3(0.0f, 0.0f, -0.5f);
        scene->Add<MeshRenderer>(cube).Mesh = assets.Adopt(mesh);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        scene->DestroyEntity(cube);
        return pixels;
    };

    const vector<u8> deferred = Render(opaqueCube);
    const vector<u8> forward = Render(translucentCube);

    // A 5x5 grid across the front face, which spans roughly ±38 texels about the centre.
    f32 maxDifference = 0.0f;
    f32 minLuma = 1e9f;
    f32 maxLuma = 0.0f;
    for (i32 dy = -2; dy <= 2; ++dy)
    {
        for (i32 dx = -2; dx <= 2; ++dx)
        {
            const auto x = static_cast<u32>(static_cast<i32>(extent.x / 2) + dx * 14);
            const auto y = static_cast<u32>(static_cast<i32>(extent.y / 2) + dy * 14);
            const vec3 lit = DecodeTexel(deferred, extent.x, x, y);
            const vec3 forwardLit = DecodeTexel(forward, extent.x, x, y);
            const vec3 difference = glm::abs(lit - forwardLit);
            maxDifference = glm::max(maxDifference,
                                     glm::max(difference.r, glm::max(difference.g, difference.b)));
            const f32 luma = glm::dot(lit, vec3(0.2126f, 0.7152f, 0.0722f));
            minLuma = glm::min(minLuma, luma);
            maxLuma = glm::max(maxLuma, luma);
        }
    }

    // The face is genuinely lit, and lit unevenly — so agreement is not two flat fills matching.
    CHECK(minLuma > 0.1f);
    CHECK(maxLuma - minLuma > 0.05f);
    // The g-buffer stores albedo as sRGB8 and roughness/metallic as 8-bit channels where the forward
    // path shades the exact floats; the tolerance covers that quantization through the tone curve.
    CHECK(maxDifference < 0.01f);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "forward lighting: a surface declining environment lighting takes the flat "
                  "ambient floor under an image-based or SH sky, deferred and forward alike")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = CookForwardPack("veng_gpu_forward_environment_flag.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const MountHandle warmMount =
        assets.MountMemory(PanoramaArchive(WarmPanorama, vec3(2.0f, 0.6f, 0.1f)), "warm sky");
    const MountHandle coolMount =
        assets.MountMemory(PanoramaArchive(CoolPanorama, vec3(0.05f, 0.3f, 1.2f)), "cool sky");
    const AssetResult<AssetHandle<EnvironmentMap>> warm =
        assets.LoadSync<EnvironmentMap>(WarmPanorama);
    const AssetResult<AssetHandle<EnvironmentMap>> cool =
        assets.LoadSync<EnvironmentMap>(CoolPanorama);
    REQUIRE(warm.has_value());
    REQUIRE(cool.has_value());

    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(AssetId{0xF34FF2CE488F7B19ULL});
    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(AssetId{0xA1F58FB895475E00ULL});
    REQUIRE(opaque.has_value());
    REQUIRE(translucent.has_value());

    const Ref<Mesh> opaqueCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *opaque), "Environment Flag Opaque");
    const Ref<Mesh> translucentCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *translucent), "Environment Flag Lit");

    // No light at all: the face's radiance is its ambient term alone.
    const Unique<Scene> scene = Scene::Create(Types);

    constexpr uvec2 extent{64, 64};
    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    const auto SetDeclines = [&](bool declines)
    {
        for (const AssetHandle<MaterialInstance>* instance : {&*opaque, &*translucent})
        {
            const_cast<MaterialInstance&>(*instance->Get())
                .SetParam("DeclinesEnvironment", declines ? 1.0f : 0.0f);
        }
    };

    // The front face's centre texel, rendered with the cube alone in the frame.
    const auto FaceColour = [&](const Ref<Mesh>& mesh) -> vec3
    {
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube).Position = vec3(0.0f, 0.0f, -0.5f);
        scene->Add<MeshRenderer>(cube).Mesh = assets.Adopt(mesh);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        scene->DestroyEntity(cube);
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    // The reference: no sky, so every surface takes the flat floor.
    SetDeclines(false);
    const vec3 floorColour = FaceColour(opaqueCube);
    REQUIRE(glm::dot(floorColour, vec3(1.0f)) > 0.01f);

    const Entity skyEntity = scene->CreateEntity();
    Sky& sky = scene->Add<Sky>(skyEntity);
    auto* source = static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()));

    f32 flaggedError = 0.0f;
    for (const SkyLighting lighting : {SkyLighting::IBL, SkyLighting::SH})
    {
        CAPTURE(static_cast<u32>(lighting));
        sky.Lighting = lighting;

        vec3 unflagged[2];
        const AssetHandle<EnvironmentMap>* maps[2] = {&*warm, &*cool};
        for (u32 i = 0; i < 2; ++i)
        {
            source->Map = *maps[i];
            SetDeclines(false);
            unflagged[i] = FaceColour(opaqueCube);

            SetDeclines(true);
            const vec3 deferred = FaceColour(opaqueCube);
            const vec3 forward = FaceColour(translucentCube);
            const vec3 error =
                glm::max(glm::abs(deferred - floorColour), glm::abs(forward - floorColour));
            flaggedError = glm::max(flaggedError, glm::max(error.r, glm::max(error.g, error.b)));
        }

        // The sky really lights an unflagged surface, and the two skies light it differently: the
        // warm sky is redder than the cool one by far more than any tolerance below.
        CHECK(unflagged[0].r - unflagged[1].r > 0.05f);
        CHECK(unflagged[1].b - unflagged[0].b > 0.02f);
    }

    // A flagged surface reads the flat floor under either sky and either arm, deferred and forward:
    // the environment's content does not reach it. The tolerance covers the g-buffer's 8-bit albedo
    // against the forward path's exact floats.
    CHECK(flaggedError < 0.01f);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "forward lighting: the environment light tint scales the sky's ambient light per "
                  "channel, deferred and forward, and leaves a surface declining it alone")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = CookForwardPack("veng_gpu_forward_environment_tint.vengpack");
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const MountHandle neutralMount =
        assets.MountMemory(PanoramaArchive(NeutralPanorama, vec3(1.0f)), "neutral sky");
    const AssetResult<AssetHandle<EnvironmentMap>> neutral =
        assets.LoadSync<EnvironmentMap>(NeutralPanorama);
    REQUIRE(neutral.has_value());

    const AssetResult<AssetHandle<MaterialInstance>> opaque =
        assets.LoadSync<MaterialInstance>(AssetId{0xF34FF2CE488F7B19ULL});
    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(AssetId{0xA1F58FB895475E00ULL});
    REQUIRE(opaque.has_value());
    REQUIRE(translucent.has_value());

    const Ref<Mesh> opaqueCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *opaque), "Environment Tint Opaque");
    const Ref<Mesh> translucentCube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *translucent), "Environment Tint Lit");

    // No light at all: the face's radiance is its ambient term alone.
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity skyEntity = scene->CreateEntity();
    Sky& sky = scene->Add<Sky>(skyEntity);
    auto* source = static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()));
    source->Map = *neutral;

    constexpr uvec2 extent{64, 64};
    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    const auto SetDeclines = [&](bool declines)
    {
        for (const AssetHandle<MaterialInstance>* instance : {&*opaque, &*translucent})
        {
            const_cast<MaterialInstance&>(*instance->Get())
                .SetParam("DeclinesEnvironment", declines ? 1.0f : 0.0f);
        }
    };

    // The front face's centre texel through the identity tone curve, so the readback is linear in
    // the ambient term.
    const auto FaceColour = [&](const Ref<Mesh>& mesh, const vec3& tint) -> vec3
    {
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube).Position = vec3(0.0f, 0.0f, -0.5f);
        scene->Add<MeshRenderer>(cube).Mesh = assets.Adopt(mesh);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .Exposure = 0.5f,
                                                           .Tonemapper = Tonemapper::None,
                                                           .EnvironmentLightTint = tint});
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        scene->DestroyEntity(cube);
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    constexpr vec3 Tint{1.0f, 0.5f, 0.25f};
    f32 scaleError = 0.0f;
    f32 declinedError = 0.0f;
    for (const SkyLighting lighting : {SkyLighting::IBL, SkyLighting::SH})
    {
        CAPTURE(static_cast<u32>(lighting));
        sky.Lighting = lighting;

        SetDeclines(false);
        for (const Ref<Mesh>& mesh : {opaqueCube, translucentCube})
        {
            const vec3 untinted = FaceColour(mesh, vec3(1.0f));
            // Lit in every channel, and short of the clamp, so the readback is the ambient term.
            REQUIRE(glm::min(untinted.r, glm::min(untinted.g, untinted.b)) > 0.05f);
            REQUIRE(glm::max(untinted.r, glm::max(untinted.g, untinted.b)) < 0.95f);
            const vec3 error = glm::abs(FaceColour(mesh, Tint) - Tint * untinted);
            scaleError = glm::max(scaleError, glm::max(error.r, glm::max(error.g, error.b)));
        }

        SetDeclines(true);
        for (const Ref<Mesh>& mesh : {opaqueCube, translucentCube})
        {
            const vec3 error = glm::abs(FaceColour(mesh, Tint) - FaceColour(mesh, vec3(1.0f)));
            declinedError = glm::max(declinedError, glm::max(error.r, glm::max(error.g, error.b)));
        }
    }

    // The tolerance covers the half-float output and the g-buffer's 8-bit albedo.
    CHECK(scaleError < 0.01f);
    CHECK(declinedError < 1e-4f);

    std::filesystem::remove(outArchive);
}
