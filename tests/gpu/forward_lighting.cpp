// The forward light loop agrees with the deferred one. A Translucent material that includes
// Veng/forward_lighting.slang evaluates the same shared lighting core the deferred pass runs, fed
// the same per-view light state from the view block, so a translucent surface at full coverage
// renders the radiance an opaque surface with identical material parameters does. The scene lights
// one cube face with a directional and a point light over the flat ambient floor, with shadows on so
// the forward draw binds the renderer's shadow and IBL sets; the opaque cube and its translucent
// twin are rendered in turn and compared across a grid of texels on that face, where the point
// light's falloff makes each texel a different shading point.

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
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "forward lighting: a full-coverage translucent surface is lit as its opaque twin")
{
    RegisterBuiltinTypes(Types);

    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_forward_lighting.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cookResult =
        cooker.CookPack(fixtureDir / "forward_lighting_pack.json", outArchive, {}, nullptr, nullptr,
                        nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE(cookResult.has_value());

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
    auto Render = [&](const Ref<Mesh>& mesh) -> vector<u8>
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
