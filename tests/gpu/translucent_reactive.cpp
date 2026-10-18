// A Translucent material declaring "reactive" writes the temporal resolve's reactive mask at location
// 2, where it draws, and a material that does not declare it leaves the mask alone. It holds with the
// bloom mask wired (location 1 bound, the material's write to it off) and without (location 1 left
// unbound), under the engine's resolve at render scale 1, the upscaling one and MetalFX's temporal
// scaler at a reduced render scale, and either way the surface still draws its colour.

#include <filesystem>
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
    constexpr uvec2 Extent{64, 64};
    constexpr AssetId PlainTranslucentId{0x0000000000896001ULL};
    constexpr AssetId ReactiveTranslucentId{0x83650A3F308B1F2FULL};

    // The reactive mask's value at the centre of the rendered sub-rect.
    f32 ReactiveAtCentre(const SceneRenderer& renderer, const f32 renderScale)
    {
        const Ref<ImageView> mask = renderer.GetReactiveMaskView();
        REQUIRE(mask != nullptr);
        const uvec3 extent = mask->GetImage()->GetExtent();
        const std::vector<u8> texels = mask->GetImage()->Download();
        const auto x = static_cast<u32>(static_cast<f32>(extent.x) * renderScale * 0.5f);
        const auto y = static_cast<u32>(static_cast<f32>(extent.y) * renderScale * 0.5f);
        return static_cast<f32>(texels[static_cast<usize>(y) * extent.x + x]) / 255.0f;
    }

    vec3 RgbAt(const std::vector<u8>& rgba16f, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "translucent reactive: a reactive translucent writes the reactive mask where it "
                  "draws, and a plain one does not")
{
    RegisterBuiltinTypes(Types);

    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_translucent_reactive.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(
                cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "translucent_pack.json", outArchive, {},
                nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> reactive =
        assets.LoadSync<MaterialInstance>(ReactiveTranslucentId);
    REQUIRE(reactive.has_value());
    CHECK(reactive->Get()->GetParent()->IsReactiveWriter());
    const AssetResult<AssetHandle<MaterialInstance>> plain =
        assets.LoadSync<MaterialInstance>(PlainTranslucentId);
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->Get()->GetParent()->IsReactiveWriter());
    // The same opaque grey as the reactive material, so the two differ only in the declaration.
    plain->Get()->SetParam("Color", vec4(0.5f, 0.5f, 0.5f, 1.0f));

    struct Drawn
    {
        f32 Reactive = 0.0f;
        f32 Color = 0.0f;
    };
    const auto draw = [&](const AssetHandle<MaterialInstance>& material,
                          const AntiAliasingMode mode, const bool bloom)
    {
        const f32 scale = mode == AntiAliasingMode::TAA ? 1.0f : 0.5f;
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Bloom = bloom, .AntiAliasing = mode, .Shadows = false, .AO = false},
        });
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 6.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        const Unique<Scene> scene = Scene::Create(Types);
        const Entity cube = scene->CreateEntity();
        scene->Add<Transform>(cube);
        scene->Add<MeshRenderer>(cube).Mesh =
            assets.Adopt(Mesh::BuildSync(Context, Primitives::Cube(2.0f, material), "Cube"));
        std::vector<u8> output;
        // A few frames, so the temporal resolve's history has settled on the cube.
        for (int frame = 0; frame < 4; ++frame)
        {
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    renderer->Execute(cmd,
                                      Veng::Renderer::SceneView{.World = *scene,
                                                                .Camera = camera,
                                                                .Delta = 0.0f,
                                                                .RenderScale = scale,
                                                                .Exposure = 1.0f,
                                                                .Tonemapper = Tonemapper::None,
                                                                // Above the grey, so bloom adds
                                                                // no glow to the measured pixel.
                                                                .BloomThreshold = 4.0f});
                });
        }
        output = renderer->GetOutput()->GetImage()->Download();
        return Drawn{.Reactive = ReactiveAtCentre(*renderer, scale),
                     .Color = RgbAt(output, Extent.x / 2, Extent.y / 2).x};
    };

    // MetalFX's temporal scaler takes the same mask; where the device has none it runs as TAAU.
    for (const AntiAliasingMode mode :
         {AntiAliasingMode::TAA, AntiAliasingMode::TAAU, AntiAliasingMode::MetalFXTemporal})
    {
        for (const bool bloom : {false, true})
        {
            CAPTURE(static_cast<u32>(mode));
            CAPTURE(bloom);
            const Drawn reactiveDrawn = draw(*reactive, mode, bloom);
            CHECK(reactiveDrawn.Reactive > 0.95f);
            CHECK(reactiveDrawn.Color == doctest::Approx(0.5f).epsilon(0.05));
            const Drawn plainDrawn = draw(*plain, mode, bloom);
            CHECK(plainDrawn.Reactive < 0.02f);
            CHECK(plainDrawn.Color == doctest::Approx(0.5f).epsilon(0.05));
        }
    }
}

#endif
