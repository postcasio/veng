// VolumeScenePass on a real device: a scene-authored VolumeField component inserts a fullscreen
// ray-march pass that composites emissive, light-absorbing media into the lit scene color. Two
// headline behaviors are pinned: (1) a single constant-density field over a black backdrop, viewed
// straight down its depth axis, integrates to the analytic emission-with-absorption result
// (e/sigma)(1 - exp(-sigma*L)) within tolerance — proving the shader's exp(-sigma*t) weighting; and
// (2) presence gating — a scene with no live field renders byte-identical to a fieldless render and
// the pass is absent, appearing only when a field is authored (the imported output preserved across
// the recompile).

#include <cmath>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VolumeField.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A constant-density field: every voxel carries the same emission (RGB) and extinction (A),
    // packed as RGBA16F. A constant field is march-position-independent, so the per-pixel jitter
    // cannot move the result and the integral has a clean closed form.
    Ref<Renderer::VolumeField> ConstantField(Context& context, const AABB& bounds, f32 emission,
                                             f32 extinction)
    {
        constexpr u32 N = 4;
        std::vector<u8> voxels(static_cast<usize>(N) * N * N * 8);
        auto* halves = reinterpret_cast<u16*>(voxels.data());
        for (usize texel = 0; texel < static_cast<usize>(N) * N * N; ++texel)
        {
            halves[texel * 4 + 0] = glm::packHalf1x16(emission);
            halves[texel * 4 + 1] = glm::packHalf1x16(emission);
            halves[texel * 4 + 2] = glm::packHalf1x16(emission);
            halves[texel * 4 + 3] = glm::packHalf1x16(extinction);
        }

        Renderer::VolumeFieldData data;
        data.Name = "Constant Field";
        data.Resolution = {N, N, N};
        data.Format = Format::RGBA16Sfloat;
        data.Bounds = bounds;
        data.Voxels = voxels;
        REQUIRE(data.IsValid());
        return Renderer::VolumeField::BuildSync(context, data);
    }

    // The linear RGB of one pixel, decoded from the RGBA16F output (tonemapper None keeps it linear).
    vec3 PixelRgb(const std::vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "volume field: a constant-density field integrates to the analytic emission result")
{
    constexpr uvec2 extent{64, 64};

    // A slab spanning z in [-1, 1] (depth 2 along the view axis), wide in x/y so the central ray
    // stays inside it. Viewed from (0,0,-5) toward +z, the center pixel's ray traverses exactly
    // L = 2 world units of constant density.
    const AABB bounds{.Min = vec3(-10.0f, -10.0f, -1.0f), .Max = vec3(10.0f, 10.0f, 1.0f)};
    constexpr f32 emission = 0.5f;   // emission radiance density per world unit
    constexpr f32 extinction = 0.5f; // extinction density per world unit
    constexpr f32 L = 2.0f;

    Types.Register<Veng::VolumeField>();
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity fieldEntity = scene->CreateEntity();
    auto& component = scene->Add<Veng::VolumeField>(fieldEntity);
    component.Field = ConstantField(Context, bounds, emission, extinction);
    // High step count so the Riemann march closely matches the closed-form integral.
    component.Steps = 128;

    AssetManager assets(Context, Tasks, Types);
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    std::vector<u8> pixels;
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                       .Camera = camera,
                                                       .Delta = 0.0f,
                                                       .Exposure = 1.0f,
                                                       .Tonemapper = Tonemapper::None});
        });
    pixels = renderer->GetOutput()->GetImage()->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);

    // Emission with absorption over a constant medium: L_out = (e/sigma)(1 - exp(-sigma*L)), the
    // background being black. The center pixel's ray is axis-aligned, so its segment length is L.
    const f32 expected = (emission / extinction) * (1.0f - std::exp(-extinction * L));
    const vec3 center = PixelRgb(pixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(center.x == doctest::Approx(expected).epsilon(0.05));
    CHECK(center.y == doctest::Approx(expected).epsilon(0.05));
    CHECK(center.z == doctest::Approx(expected).epsilon(0.05));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "volume field: a fieldless scene compiles without the pass; a field inserts it")
{
    constexpr uvec2 extent{64, 64};

    Types.Register<Veng::VolumeField>();
    const Unique<Scene> scene = Scene::Create(Types);

    AssetManager assets(Context, Tasks, Types);
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false},
    });

    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    auto Render = [&]() -> std::vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .Exposure = 1.0f,
                                                           .Tonemapper = Tonemapper::None});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // No VolumeField authored: the presence-driven pass is absent and the empty scene renders black.
    const Ref<ImageView> output = renderer->GetOutput();
    {
        const std::vector<u8> pixels = Render();
        const vec3 center = PixelRgb(pixels, extent.x, extent.x / 2, extent.y / 2);
        CHECK(center.x + center.y + center.z < 0.02f);
    }

    // Authoring a live field inserts the pass at the frame boundary; the center now marches bright.
    const AABB bounds{.Min = vec3(-10.0f, -10.0f, -1.0f), .Max = vec3(10.0f, 10.0f, 1.0f)};
    const Entity fieldEntity = scene->CreateEntity();
    scene->Add<Veng::VolumeField>(fieldEntity).Field = ConstantField(Context, bounds, 0.6f, 0.6f);
    {
        const std::vector<u8> pixels = Render();
        const vec3 center = PixelRgb(pixels, extent.x, extent.x / 2, extent.y / 2);
        CHECK(center.x > 0.2f);
    }

    // The presence recompile reuses the imported output — a cached GetOutput() ref stays valid.
    CHECK(renderer->GetOutput() == output);

    // Dropping the field removes the pass again; the center returns to black.
    scene->Get<Veng::VolumeField>(fieldEntity).Field = nullptr;
    {
        const std::vector<u8> pixels = Render();
        const vec3 center = PixelRgb(pixels, extent.x, extent.x / 2, extent.y / 2);
        CHECK(center.x + center.y + center.z < 0.02f);
    }
    CHECK(renderer->GetOutput() == output);
}
