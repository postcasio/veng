// A Translucent material passing its colour through CompensateExposure (Veng/translucent.slang)
// renders the same at any exposure, where a plain one scales with it: the view block carries the
// frame's exposure, and the helper divides it back out.

#include <filesystem>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"
#include "support/TestCook.h"

#ifdef GPU_GBUFFER_FIXTURE_DIR

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};
    constexpr AssetId PlainTranslucentId{0x0000000000896001ULL};
    constexpr AssetId CompensatedTranslucentId{0x7695FF53569A9832ULL};

    f32 RedAtCentre(const std::vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(Extent.y / 2) * Extent.x + Extent.x / 2) * 4;
        return glm::unpackHalf1x16(halves[base]);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "translucent exposure: a compensating material keeps its brightness at any "
                  "exposure")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_translucent_exposure.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(
                cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "translucent_pack.json", outArchive, {},
                nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> compensated =
        assets.LoadSync<MaterialInstance>(CompensatedTranslucentId);
    REQUIRE(compensated.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> plain =
        assets.LoadSync<MaterialInstance>(PlainTranslucentId);
    REQUIRE(plain.has_value());
    // Dim enough that four times it stays inside the output's range.
    plain->Get()->SetParam("Color", vec4(0.2f, 0.2f, 0.2f, 1.0f));

    const auto draw = [&](const AssetHandle<MaterialInstance>& material, const f32 exposure)
    {
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Bloom = false, .Shadows = false, .AO = false},
        });
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 6.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        const Unique<Scene> scene = Scene::Create(Types);
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube);
        scene->Add<MeshRenderer>(cube).Mesh =
            assets.Adopt(Mesh::BuildSync(Context, Primitives::Cube(2.0f, material), "Cube"));
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Veng::Renderer::SceneView{.World = *scene,
                                                                 .Camera = camera,
                                                                 .Delta = 0.0f,
                                                                 .Exposure = exposure,
                                                                 .Tonemapper = Tonemapper::None});
            });
        return RedAtCentre(renderer->GetOutput()->GetImage()->Download());
    };

    const f32 plainLow = draw(plain.value(), 1.0f);
    const f32 plainHigh = draw(plain.value(), 4.0f);
    CHECK(plainLow == doctest::Approx(0.2f).epsilon(0.03));
    CHECK(plainHigh == doctest::Approx(plainLow * 4.0f).epsilon(0.03));

    const f32 heldLow = draw(compensated.value(), 1.0f);
    const f32 heldHigh = draw(compensated.value(), 4.0f);
    CHECK(heldLow == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(heldHigh == doctest::Approx(heldLow).epsilon(0.01));

    std::filesystem::remove(outArchive);
}

#endif
