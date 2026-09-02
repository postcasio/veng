// The depth-of-field battery on a real device. A brick cube sits at a known distance against the
// cleared background, so the frame carries both a large in-focus interior and a sharp silhouette
// the defocus visibly spills across. Three renders of the identical scene pin the effect:
//
//  - depth of field off, the reference;
//  - on, focused at the cube's own distance — the cube interior must read the reference value
//    within tolerance, because a zero circle of confusion composites as the untouched source;
//  - on, focused far past the cube — the cube is heavily defocused, so background texels just
//    outside its silhouette pick up its radiance, and the whole frame differs from the focused
//    capture.
//
// The battery is off by default, so the first render is also the standing proof that the shipping
// path is the one every other capture already asserts against.

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <cmath>

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

#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // One RGBA16F output texel decoded to a linear vec3 (the renderer output is RGBA16Sfloat and
    // the test drives the tonemapper to None, so the value is the linear radiance).
    vec3 DofTexel(const vector<u8>& rgba16f, const u32 width, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    f32 DofLuminance(const vec3 color)
    {
        return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
    }

    // Mean absolute luminance difference over the whole frame — the coarse "did the image move"
    // measure the two focus distances are compared with.
    f64 MeanAbsoluteDifference(const vector<u8>& a, const vector<u8>& b, const uvec2 extent)
    {
        f64 sum = 0.0;
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                sum += std::abs(DofLuminance(DofTexel(a, extent.x, x, y)) -
                                DofLuminance(DofTexel(b, extent.x, x, y)));
            }
        }
        return sum / (static_cast<f64>(extent.x) * extent.y);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "depth of field: an in-focus region is unchanged and a defocused one spills")
{
    constexpr uvec2 extent{128, 128};
    // The camera sits 5 units from the origin and the cube is 1.4 across, so its front face is
    // 4.3 units away — the distance the focused capture focuses at. At a 45 degree vertical field
    // of view the cube covers the middle ~40% of the frame, leaving a wide background margin above
    // its silhouette for the near-field spill to land in.
    constexpr f32 CubeFaceDistance = 4.3f;

    RegisterBuiltinTypes(Types);

    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_dof.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cookResult =
        cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
                        nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> material =
        assets.LoadSync<MaterialInstance>(AssetId{0x895443}); // the brick default instance
    REQUIRE(material.has_value());
    REQUIRE(material->IsLoaded());

    const Ref<Mesh> cube = Mesh::BuildSync(Context, Primitives::Cube(1.4f, *material), "DoF Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f),
        .Color = vec3(1.0f, 1.0f, 1.0f),
        // A directional's intensity is an illuminance in lux (internal radiance 1.0 at the anchor).
        .Intensity = 1.0f / Renderer::LuminousAnchor,
    };

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Renders the identical scene through a renderer built with the given settings, at the given
    // focus distance. Bloom and the other batteries stay off so the only variable is the defocus.
    auto Render = [&](const bool depthOfField, const f32 focusDistance) -> vector<u8>
    {
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = extent,
            .Settings = {.Mode = DebugView::Final,
                         .Bloom = false,
                         .Shadows = false,
                         .DepthOfField = depthOfField,
                         .AO = false},
        });
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{
                                           .World = *scene,
                                           .Camera = camera,
                                           .Delta = 0.0f,
                                           .Exposure = 1.0f,
                                           .Tonemapper = Tonemapper::None,
                                           .DofFocusDistance = focusDistance,
                                       });
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    const vector<u8> reference = Render(false, CubeFaceDistance);
    const vector<u8> focused = Render(true, CubeFaceDistance);
    const vector<u8> defocused = Render(true, 100.0f);

    // The cube interior: far enough inside the silhouette that the dilated tile ring around it
    // holds nothing but front-face texels, so its circle of confusion is zero in the focused
    // capture and the composite hands the source value straight through.
    const vec3 referenceCenter = DofTexel(reference, extent.x, extent.x / 2, extent.y / 2);
    const vec3 focusedCenter = DofTexel(focused, extent.x, extent.x / 2, extent.y / 2);
    REQUIRE(DofLuminance(referenceCenter) > 0.1f);
    CHECK(focusedCenter.r == doctest::Approx(referenceCenter.r).epsilon(0.02));
    CHECK(focusedCenter.g == doctest::Approx(referenceCenter.g).epsilon(0.02));
    CHECK(focusedCenter.b == doctest::Approx(referenceCenter.b).epsilon(0.02));

    // A background texel just outside the cube's top silhouette. With the cube defocused its
    // near-field spills across the sharp geometry behind it, so the texel brightens well above the
    // reference — the defocused region is provably changed.
    const vec3 referenceEdge = DofTexel(reference, extent.x, extent.x / 2, 30);
    const vec3 defocusedEdge = DofTexel(defocused, extent.x, extent.x / 2, 30);
    CHECK(DofLuminance(defocusedEdge) > DofLuminance(referenceEdge) + 0.02f);

    // Two captures at different focus distances are different images.
    const f64 focusDelta = MeanAbsoluteDifference(focused, defocused, extent);
    CHECK(focusDelta > 0.005);

    // ...and the focused capture is the near-reference one: focusing at the subject moves far less
    // than focusing past it.
    const f64 focusedDelta = MeanAbsoluteDifference(reference, focused, extent);
    const f64 defocusedDelta = MeanAbsoluteDifference(reference, defocused, extent);
    CHECK(focusedDelta < defocusedDelta);

    // The debug arm force-wires the chain's first two stages with the feature off, so the
    // circle-of-confusion and tile targets are produced and the graph is valid without the gather,
    // fill, or composite ever running.
    const Unique<SceneRenderer> debugRenderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::CoC,
                     .Bloom = false,
                     .Shadows = false,
                     .DepthOfField = false,
                     .AO = false},
    });
    auto RenderDebug = [&](const f32 maxCoc)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                debugRenderer->Execute(cmd, Renderer::SceneView{
                                                .World = *scene,
                                                .Camera = camera,
                                                .Delta = 0.0f,
                                                .Exposure = 1.0f,
                                                .Tonemapper = Tonemapper::None,
                                                .DofFocusDistance = CubeFaceDistance,
                                                .DofMaxCoc = maxCoc,
                                            });
            });
        const vector<u8> pixels = debugRenderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    const vector<u8> debugPixels = RenderDebug(16.0f);

    // The arm blits the signed circle of confusion: the near field ramps red, the far field blue,
    // and an in-focus texel is black. The cube's front face is exactly the focus plane, so the
    // frame centre is black; the cleared background sits at the far plane, far past focus, so it
    // saturates the far ramp.
    const vec3 focusPlane = DofTexel(debugPixels, extent.x, extent.x / 2, extent.y / 2);
    CHECK(focusPlane.r < 0.02f);
    CHECK(focusPlane.b < 0.02f);

    const vec3 background = DofTexel(debugPixels, extent.x, 4, 4);
    CHECK(background.b > 0.2f);
    CHECK(background.r < 0.02f);

    // The ramp normalizes against the frame's own budget, so halving the budget brightens the same
    // geometry's far ramp — the visualization tracks what is being tuned rather than a fixed
    // ceiling.
    auto MeanFar = [&](const vector<u8>& pixels)
    {
        f64 sum = 0.0;
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                sum += DofTexel(pixels, extent.x, x, y).b;
            }
        }
        return sum / (static_cast<f64>(extent.x) * extent.y);
    };
    const vector<u8> shallowPixels = RenderDebug(4.0f);
    CHECK(MeanFar(shallowPixels) > MeanFar(debugPixels));
}

#endif
