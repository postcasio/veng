// Debug-draw cases. The renderer's DebugDrawScenePass flushes the per-renderer DebugDraw
// accumulator into the LDR scene color after tonemap, depth-aware via a g-buffer depth
// sample. The headline case proves the depth-fade is correct UNDER DYNAMIC RESOLUTION: it
// renders a brick cube at a sub-1.0 RenderScale (the g-buffer lives in a sub-rect of its
// allocation) and draws a debug line spanning the frame BEHIND the cube. The line fades
// where the cube occludes it and stays bright where it does not — which only holds if the
// depth sample remaps the logical UV by RenderScaleUV/MaxValidUV onto the right sub-rect
// depth texel. A wrong remap would read depth at the wrong place and misplace the fade.
//
// The cube must write depth for the occlusion test to fire, so it carries a real material —
// the case is cooker-gated, reusing the brick g-buffer fixture (a Surface material on the
// core canonical layout).

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <algorithm>
#include <cmath>
#include <filesystem>

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
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The peak luminance of a thin debug line within a small vertical band around row,
    // so a one-texel rasterization offset never moves the line off the sampled row.
    f32 BandPeak(const vector<u8>& rgba16f, u32 width, u32 height, u32 column, u32 row)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        f32 peak = 0.0f;
        for (i32 dy = -3; dy <= 3; ++dy)
        {
            const i32 y = static_cast<i32>(row) + dy;
            if (y < 0 || y >= static_cast<i32>(height))
            {
                continue;
            }
            const usize base = (static_cast<usize>(y) * width + column) * 4;
            const f32 r = glm::unpackHalf1x16(halves[base + 0]);
            const f32 g = glm::unpackHalf1x16(halves[base + 1]);
            const f32 b = glm::unpackHalf1x16(halves[base + 2]);
            peak = std::max(peak, 0.2126f * r + 0.7152f * g + 0.0722f * b);
        }
        return peak;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "debug draw: the depth-aware occluded fade is correct under dynamic resolution")
{
    RegisterBuiltinTypes(Types);

    // Cook the brick g-buffer fixture so the cube writes depth (the occlusion test reads it).
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_debugdraw.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    // The brick shaders `#include "Veng/material.slang"`; the engine core shader dir is on
    // the cook's Slang search path so the cross-pack include resolves.
    const VoidResult cookResult =
        cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
                        nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<MaterialInstance>> material =
        assets.LoadSync<MaterialInstance>(AssetId{0x895443}); // the brick default instance
    REQUIRE(material.has_value());

    constexpr uvec2 extent{160, 160};

    // A brick cube fills the center of the view: the geometry pass writes its depth (the debug
    // pass samples it) and its front face is at world z = +1.
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(2.0f, *material), "Debug Cube");
    const Entity cubeEntity = scene->CreateEntity();
    scene->Add<Transform>(cubeEntity);
    scene->Add<MeshRenderer>(cubeEntity).Mesh = assets.Adopt(cube);

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f),
                          static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 4.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Bloom = false, .Shadows = false, .AO = false, .DebugDraw = true},
    });

    // Render the debug scene at the given RenderScale and return the downloaded output.
    auto RenderAt = [&](f32 renderScale) -> vector<u8>
    {
        // The accumulator clears each Execute, so push fresh every frame. A bright white line at
        // y = 0 behind the cube (world z = -1, further from the camera than the cube's front face
        // at z = +1) spanning x in [-6, 6]: its center sits behind the cube (occluded → faded),
        // its visible flanks miss the cube silhouette (the far depth clear → not occluded → bright).
        renderer->GetDebugDraw().DrawLine(vec3(-6.0f, 0.0f, -1.0f), vec3(6.0f, 0.0f, -1.0f),
                                          vec4(1.0f, 1.0f, 1.0f, 1.0f));

        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .RenderScale = renderScale});
            });
        vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    // The line projects to the middle scanline. Sample the line's peak luminance at the center
    // column (over the cube) and near an edge column (off the cube silhouette).
    const u32 row = extent.y / 2;
    const u32 centerCol = extent.x / 2;
    const u32 edgeCol = 8;

    SUBCASE("native resolution: occluded center fades, unoccluded edge stays bright")
    {
        const vector<u8> pixels = RenderAt(1.0f);
        const f32 center = BandPeak(pixels, extent.x, extent.y, centerCol, row);
        const f32 edge = BandPeak(pixels, extent.x, extent.y, edgeCol, row);

        // The unoccluded flank of the line is bright white; the occluded center is the dim fade.
        CHECK(edge > 0.6f);
        CHECK(center < edge * 0.6f);
    }

    SUBCASE("dynamic resolution (scale 0.6): the sub-rect depth remap keeps the fade correct")
    {
        // The g-buffer renders into a 0.6 sub-rect of the allocation; the debug pass samples its
        // depth through the RenderScaleUV/MaxValidUV remap. If the remap were wrong the center
        // would not read the cube's depth and the line would not fade behind it.
        const vector<u8> pixels = RenderAt(0.6f);
        const f32 center = BandPeak(pixels, extent.x, extent.y, centerCol, row);
        const f32 edge = BandPeak(pixels, extent.x, extent.y, edgeCol, row);

        CHECK(edge > 0.6f);
        CHECK(center < edge * 0.6f);
    }

    std::filesystem::remove(outArchive);
}

#endif // GPU_GBUFFER_FIXTURE_DIR
