// A Translucent material declaring "excludedFromMetering" keeps the auto-exposure meter from reading
// the pixels it flags, as an opaque surface's flags do. A bright translucent cube and a lit opaque one
// share the view, under the highlight cap: drawn with a plain material, the bright cube is the
// frame's highlight and holds the exposure down, so the lit cube renders dim; drawn with the
// excluding material, the meter reads only the lit cube and exposes for it. The bright cube is drawn
// either way, and brighter once it no longer holds the exposure.

#include <filesystem>
#include <utility>
#include <vector>

#include <glm/gtc/packing.hpp>

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

using namespace Veng;
using namespace Veng::Renderer;

#ifdef GPU_GBUFFER_FIXTURE_DIR

namespace
{
    constexpr uvec2 Extent{128, 128};
    constexpr AssetId LitBrickId{0x0000000000895443ULL};
    constexpr AssetId PlainTranslucentId{0x0000000000896001ULL};
    constexpr AssetId MeteringTranslucentId{0x7E6580614F0CE354ULL};

    f32 LumaAt(const std::vector<u8>& rgba16f, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
        const vec3 c(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                     glm::unpackHalf1x16(halves[base + 2]));
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "translucent metering: a translucent excluded from metering does not set the "
                  "exposure")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_translucent_metering.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(
                cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "translucent_pack.json", outArchive, {},
                nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> lit =
        assets.LoadSync<MaterialInstance>(LitBrickId);
    REQUIRE(lit.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> plain =
        assets.LoadSync<MaterialInstance>(PlainTranslucentId);
    REQUIRE(plain.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> excluding =
        assets.LoadSync<MaterialInstance>(MeteringTranslucentId);
    REQUIRE(excluding.has_value());
    CHECK(excluding->Get()->GetParent()->IsMeteringExcluder());
    CHECK_FALSE(plain->Get()->GetParent()->IsMeteringExcluder());
    // The same opaque white glow in both, so the two differ only in the declaration.
    for (const AssetHandle<MaterialInstance>& material : {*plain, *excluding})
    {
        material.Get()->SetParam("Color", vec4(200.0f, 200.0f, 200.0f, 1.0f));
    }

    const auto Converge = [&](const AssetHandle<MaterialInstance>& glow) -> std::pair<f32, f32>
    {
        const Unique<Scene> scene = Scene::Create(Types);
        const auto AddCube = [&](const vec3 at, const AssetHandle<MaterialInstance>& material)
        {
            const Entity entity = scene->CreateEntity();
            scene->Add<Transform>(entity).Position = at;
            scene->Add<MeshRenderer>(entity).Mesh =
                assets.Adopt(Mesh::BuildSync(Context, Primitives::Cube(1.0f, material), "Cube"));
        };
        AddCube(vec3(-0.65f, 0.0f, 0.0f), glow);
        AddCube(vec3(0.65f, 0.0f, 0.0f), *lit);
        scene->Add<Light>(scene->CreateEntity()) = Light{.Direction = vec3(0.0f, 0.0f, -1.0f),
                                                         .Color = vec3(1.0f),
                                                         .Intensity = 1.0f / LuminousAnchor};

        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.6f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final,
                         .Bloom = false,
                         .Shadows = false,
                         .AO = false,
                         .AutoExposure = true},
        });
        // The meter is read frames-in-flight late, so the adaptation is given time to converge.
        for (u32 frame = 0; frame < 24; ++frame)
        {
            CommandBuffer& cmd = Context.BeginFrame();
            renderer->Execute(cmd, Renderer::SceneView{
                                       .World = *scene,
                                       .Camera = camera,
                                       .Delta = 1.0f,
                                       .AutoExposureMaxLuminance = 1000.0f,
                                       .AutoExposureSpeed = 4.0f,
                                       .AutoExposureHighlightPercentile = 0.9f,
                                       .AutoExposureHighlightLuminance = 1.0f,
                                   });
            Context.EndFrame();
            Context.WaitIdle();
        }
        const std::vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        return {LumaAt(pixels, Extent.x / 2 - 22, Extent.y / 2),
                LumaAt(pixels, Extent.x / 2 + 22, Extent.y / 2)};
    };

    const auto [glowMetered, litBesideGlow] = Converge(*plain);
    const auto [glowExcluded, litAlone] = Converge(*excluding);

    // Metered, the glow sets the exposure and the lit cube sits far below it.
    CHECK(litBesideGlow < 0.5f * glowMetered);
    // Excluded, the meter exposes for the lit cube, and the glow is drawn brighter still.
    CHECK(litAlone > 2.0f * litBesideGlow);
    CHECK(glowExcluded > glowMetered);

    std::filesystem::remove(outArchive);
}

#endif
