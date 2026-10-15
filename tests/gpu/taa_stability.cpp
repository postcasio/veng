// The temporal resolve under a static camera. TAA and TAAU jitter the projection by a sub-pixel
// offset each frame, and the resolve has to take that offset back out: with nothing moving, a
// converged resolve holds still. Each case renders a brick cube with an emissive face over a
// gradient sky, plus a SceneColor-placed point-field star, at render scale 1 and 0.5 (and once
// under TAA with a depth-of-field composite after the resolve), and after three jitter periods
// asserts that
//  - the motion vectors are zero (nothing moved, so nothing may reproject);
//  - frame to frame, the resolved HDR changes by less than a small bound inside the cube
//    (depth > 0) and in the open sky (depth == 0);
//  - the star's centroid holds its position to within 0.05 output px, and the cube's silhouette to
//    within the ripple a hard edge's per-frame coverage leaves in the history (0.1 output px).
//
// The silhouette band and the star's own footprint are excluded from the per-pixel bound: a pixel
// an edge crosses is where the jitter sequence is being integrated, so it legitimately moves by a
// fraction of the edge contrast each frame. What may not move is where that edge sits, which the
// centroid checks hold.
//
// MetalFX's temporal scaler is held to the same claims on looser bounds, since its reconstruction
// is the platform's rather than the engine's; and its spatial scaler, which keeps no history, to
// placing a half-scale render where the native one lies.

#include <algorithm>
#include <cmath>
#include <filesystem>

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
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/TaaJitter.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId BrickInstanceId{0x0000000000895443ULL};
    constexpr AssetId AnalyticSkyInstanceId{0x00000000000024A1ULL};

    // Small enough that a case stays cheap, with a wide field of view so the sky's gradient is
    // steep per pixel: a resolve that lets the background drift by a fraction of a pixel then
    // changes it by many half-float steps, well clear of quantization.
    constexpr uvec2 Extent{64, 64};

    // Three periods converge the exponential history; one more period plus a frame is measured,
    // so every jitter phase appears in a measured frame-to-frame step.
    constexpr u32 SettleFrames = 3 * TaaJitterSampleCount;
    constexpr u32 MeasuredFrames = TaaJitterSampleCount + 1;

    // Output pixels within this many render pixels of the cube's silhouette are the edge band (a
    // render pixel spans 1 / renderScale output pixels); the star's footprint is excluded within
    // this radius of its peak.
    constexpr f32 EdgeBandRenderPixels = 2.0f;
    constexpr i32 StarRadius = 3;
    constexpr i32 BorderBand = 2;

    // A luminance above this is the cube (its emissive face is far above the sky's ceiling).
    constexpr f32 CubeThreshold = 0.7f;

    struct HdrFrame
    {
        vector<vec3> Rgb;
        vector<f32> Luminance;
    };

    HdrFrame Decode(const vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize count = static_cast<usize>(Extent.x) * Extent.y;
        HdrFrame frame;
        frame.Rgb.resize(count);
        frame.Luminance.resize(count);
        for (usize i = 0; i < count; ++i)
        {
            const vec3 c(glm::unpackHalf1x16(halves[i * 4 + 0]),
                         glm::unpackHalf1x16(halves[i * 4 + 1]),
                         glm::unpackHalf1x16(halves[i * 4 + 2]));
            frame.Rgb[i] = c;
            frame.Luminance[i] = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        }
        return frame;
    }

    usize Index(const i32 x, const i32 y)
    {
        return static_cast<usize>(y) * Extent.x + static_cast<usize>(x);
    }

    // The worst one-frame change of the resolved image, and the spread of the two centroids, over
    // the measured frames.
    struct Stability
    {
        f32 CubeDelta = 0.0f;
        f32 SkyDelta = 0.0f;
        f32 EdgeWander = 0.0f;
        f32 StarWander = 0.0f;
        u32 CubePixels = 0;
        u32 SkyPixels = 0;
    };

    // The largest distance of any sample from the samples' mean.
    f32 Wander(const vector<f32>& samples)
    {
        f32 mean = 0.0f;
        for (const f32 s : samples)
        {
            mean += s;
        }
        mean /= static_cast<f32>(samples.size());
        f32 worst = 0.0f;
        for (const f32 s : samples)
        {
            worst = std::max(worst, std::abs(s - mean));
        }
        return worst;
    }

    struct StabilityScene
    {
        explicit StabilityScene(Test::GpuFixture& fixture)
            : Gpu(fixture), Assets(fixture.Context, fixture.Tasks, fixture.Types)
        {
            RegisterBuiltinTypes(fixture.Types);
            fixture.Types.Register<Veng::PointField>();

            Cook::Cooker cooker;
            Cook::RegisterBuiltinImporters(cooker);
            Archive = Veng::TestSupport::TempDir() / "veng_gpu_taa_stability.vengpack";
            REQUIRE(Veng::TestSupport::CookCached(
                        cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "gbuffer_pack.json", Archive, {},
                        nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                        .has_value());
            REQUIRE(Assets.Mount(Archive).has_value());

            const AssetResult<AssetHandle<MaterialInstance>> brick =
                Assets.LoadSync<MaterialInstance>(BrickInstanceId);
            REQUIRE(brick.has_value());
            // A dim brick texture under a bright emissive face: the silhouette is a clean step
            // against the sky, and the interior still carries texture a drifting resolve would move.
            brick->Get()->SetParam("BaseColorFactor", vec4(0.3f, 0.3f, 0.3f, 1.0f));
            brick->Get()->SetParam("EmissiveFactor", vec4(1.0f, 1.0f, 1.0f, 0.0f));
            const AssetResult<AssetHandle<MaterialInstance>> sky =
                Assets.LoadSync<MaterialInstance>(AnalyticSkyInstanceId);
            REQUIRE(sky.has_value());
            // The sky is 0.5 + 0.5 * direction, scaled down so it stays under the cube threshold.
            sky->Get()->SetParam("Scale", vec4(0.4f, 0.4f, 0.4f, 1.0f));

            World = Scene::Create(fixture.Types);

            const Ref<Mesh> cube = Mesh::BuildSync(fixture.Context, Primitives::Cube(1.4f, *brick),
                                                   "TAA Stability Cube");
            const Entity cubeEntity = World->CreateEntity();
            World->Add<Transform>(cubeEntity).Position = vec3(0.0f, 0.0f, -3.0f);
            World->Add<MeshRenderer>(cubeEntity).Mesh = Assets.Adopt(cube);

            World->Add<Light>(World->CreateEntity()) = Light{
                .Direction = vec3(0.0f, 0.0f, -1.0f),
                .Color = vec3(1.0f),
                .Intensity = 1.0f / LuminousAnchor,
            };

            Sky& skyComponent = World->Add<Sky>(World->CreateEntity());
            auto* source =
                static_cast<MaterialSky*>(skyComponent.Source.SetActive(TypeIdOf<MaterialSky>()));
            source->Material = *sky;
            source->Mode = SkyMode::Direct;

            // One star well clear of the cube, sized to a couple of pixels so it is a point the
            // jitter moves across texels rather than a disc covering them.
            const vec3 starDirection = glm::normalize(vec3(0.7f, -0.6f, -1.0f));
            const FieldPoint star{
                .Position = starDirection * 400.0f, .ColorRgba8 = 0xFFFFFFFFu, .Size = 40.0f};
            auto field = Renderer::PointField::Create(
                fixture.Context,
                {.Name = "TAA Stability Star", .Points = {&star, 1}, .CellSize = 1000.0f});
            auto& fieldComponent = World->Add<Veng::PointField>(World->CreateEntity());
            fieldComponent.Lod = Renderer::PointFieldLod{.AggregateThreshold = 1.0e6f};
            fieldComponent.Placement = PointFieldPlacement::SceneColor;
            fieldComponent.Field = std::move(field);

            Camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
            Camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
        }

        ~StabilityScene() { std::filesystem::remove(Archive); }

        StabilityScene(const StabilityScene&) = delete;
        StabilityScene& operator=(const StabilityScene&) = delete;

        void Execute(SceneRenderer& renderer, const f32 renderScale)
        {
            Gpu.Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    // A zero aperture: a wired depth-of-field chain runs every stage and composites
                    // without defocusing, so the case measures the resolve's routing through the
                    // chain's intermediate rather than the blur.
                    renderer.Execute(cmd, Renderer::SceneView{.World = *World,
                                                              .Camera = Camera,
                                                              .Delta = 0.0f,
                                                              .RenderScale = renderScale,
                                                              .Exposure = 1.0f,
                                                              .DofAperture = 0.0f});
                });
        }

        // The largest motion-vector magnitude the debug arm shows for this scene: its blit is black
        // exactly where velocity is zero.
        f32 MaxMotion(const AntiAliasingMode mode)
        {
            const Unique<SceneRenderer> renderer = SceneRenderer::Create({
                .Context = Gpu.Context,
                .Assets = Assets,
                .OutputFormat = Gpu.Context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = {.Mode = DebugView::MotionVectors,
                             .Bloom = false,
                             .AntiAliasing = mode,
                             .Shadows = false,
                             .PunctualShadows = false,
                             .AO = false},
            });
            Execute(*renderer, 1.0f);
            Execute(*renderer, 1.0f);
            const HdrFrame frame = Decode(renderer->GetOutput()->GetImage()->Download());
            f32 worst = 0.0f;
            for (const vec3& c : frame.Rgb)
            {
                worst = std::max({worst, c.r, c.g, c.b});
            }
            return worst;
        }

        // The settings a renderer runs when asked for these, after any engine fallback.
        SceneRendererSettings Resolved(const SceneRendererSettings& settings)
        {
            const Unique<SceneRenderer> renderer = SceneRenderer::Create({
                .Context = Gpu.Context,
                .Assets = Assets,
                .OutputFormat = Gpu.Context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = settings,
            });
            return renderer->GetSettings();
        }

        // The tonemapped output after a few frames under the given settings and render scale.
        HdrFrame Output(const SceneRendererSettings& settings, const f32 renderScale)
        {
            const Unique<SceneRenderer> renderer = SceneRenderer::Create({
                .Context = Gpu.Context,
                .Assets = Assets,
                .OutputFormat = Gpu.Context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = settings,
            });
            // The promotion is wired by the first frame rendering below the allocation; the frames
            // after it run the promoted graph.
            for (u32 frame = 0; frame < 3; ++frame)
            {
                Execute(*renderer, renderScale);
            }
            return Decode(renderer->GetOutput()->GetImage()->Download());
        }

        Stability Measure(const AntiAliasingMode mode, const f32 renderScale,
                          const bool depthOfField = false)
        {
            const Unique<SceneRenderer> renderer = SceneRenderer::Create({
                .Context = Gpu.Context,
                .Assets = Assets,
                .OutputFormat = Gpu.Context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = {.Mode = DebugView::Final,
                             .Bloom = false,
                             .AntiAliasing = mode,
                             .Shadows = false,
                             .PunctualShadows = false,
                             .DepthOfField = depthOfField,
                             .AO = false},
            });

            for (u32 frame = 0; frame < SettleFrames; ++frame)
            {
                Execute(*renderer, renderScale);
            }
            vector<HdrFrame> frames;
            frames.reserve(MeasuredFrames);
            for (u32 frame = 0; frame < MeasuredFrames; ++frame)
            {
                Execute(*renderer, renderScale);
                REQUIRE(renderer->GetHdrView()->GetImage()->GetWidth() == Extent.x);
                frames.push_back(Decode(renderer->GetHdrView()->GetImage()->Download()));
            }

            const i32 edgeBand = static_cast<i32>(std::ceil(EdgeBandRenderPixels / renderScale));

            // Classify against the first measured frame: the cube, then its eroded interior and the
            // sky clear of its dilated silhouette, the star's peak, and the image border.
            const HdrFrame& first = frames.front();
            const i32 w = static_cast<i32>(Extent.x);
            const i32 h = static_cast<i32>(Extent.y);
            const auto IsCube = [&](const i32 x, const i32 y)
            {
                return first.Luminance[Index(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1))] >
                       CubeThreshold;
            };
            const auto NearEdge = [&](const i32 x, const i32 y)
            {
                const bool self = IsCube(x, y);
                for (i32 dy = -edgeBand; dy <= edgeBand; ++dy)
                {
                    for (i32 dx = -edgeBand; dx <= edgeBand; ++dx)
                    {
                        if (IsCube(x + dx, y + dy) != self)
                        {
                            return true;
                        }
                    }
                }
                return false;
            };

            // The star is the brightest sky pixel clear of the cube.
            ivec2 star{-1, -1};
            f32 starPeak = 0.0f;
            for (i32 y = BorderBand + StarRadius; y < h - BorderBand - StarRadius; ++y)
            {
                for (i32 x = BorderBand + StarRadius; x < w - BorderBand - StarRadius; ++x)
                {
                    if (!IsCube(x, y) && !NearEdge(x, y) && first.Luminance[Index(x, y)] > starPeak)
                    {
                        starPeak = first.Luminance[Index(x, y)];
                        star = {x, y};
                    }
                }
            }
            REQUIRE(star.x >= 0);

            Stability result;
            vector<u8> region(static_cast<usize>(w) * h, 0); // 1 cube interior, 2 open sky
            for (i32 y = BorderBand; y < h - BorderBand; ++y)
            {
                for (i32 x = BorderBand; x < w - BorderBand; ++x)
                {
                    if (NearEdge(x, y) ||
                        (std::abs(x - star.x) <= StarRadius && std::abs(y - star.y) <= StarRadius))
                    {
                        continue;
                    }
                    region[Index(x, y)] = IsCube(x, y) ? 1 : 2;
                    ++(IsCube(x, y) ? result.CubePixels : result.SkyPixels);
                }
            }

            // The cube's left silhouette: the first cube pixel along the middle row, measured over
            // the middle third of the cube's rows.
            i32 edgeX = -1;
            i32 cubeTop = h;
            i32 cubeBottom = -1;
            for (i32 x = 0; x < w && edgeX < 0; ++x)
            {
                if (IsCube(x, h / 2))
                {
                    edgeX = x;
                }
            }
            REQUIRE(edgeX > edgeBand + 2);
            for (i32 y = 0; y < h; ++y)
            {
                if (IsCube(w / 2, y))
                {
                    cubeTop = std::min(cubeTop, y);
                    cubeBottom = std::max(cubeBottom, y);
                }
            }
            const i32 bandTop = cubeTop + (cubeBottom - cubeTop) / 3;
            const i32 bandBottom = cubeBottom - (cubeBottom - cubeTop) / 3;

            vector<f32> edges;
            vector<f32> starsX;
            vector<f32> starsY;
            for (usize f = 0; f < frames.size(); ++f)
            {
                const HdrFrame& frame = frames[f];
                if (f > 0)
                {
                    const HdrFrame& prior = frames[f - 1];
                    for (usize i = 0; i < region.size(); ++i)
                    {
                        if (region[i] == 0)
                        {
                            continue;
                        }
                        const vec3 d = glm::abs(frame.Rgb[i] - prior.Rgb[i]);
                        const f32 delta = std::max({d.r, d.g, d.b});
                        f32& worst = region[i] == 1 ? result.CubeDelta : result.SkyDelta;
                        worst = std::max(worst, delta);
                    }
                }

                // The edge sits at the luminance-rise-weighted mean of the pixel boundaries across
                // the silhouette; boundary x + 1 lies between pixels x and x + 1.
                f64 edgeSum = 0.0;
                f64 edgeWeight = 0.0;
                for (i32 y = bandTop; y <= bandBottom; ++y)
                {
                    for (i32 x = edgeX - edgeBand - 2; x < edgeX + edgeBand + 1; ++x)
                    {
                        const f32 rise = std::max(
                            frame.Luminance[Index(x + 1, y)] - frame.Luminance[Index(x, y)], 0.0f);
                        edgeSum += static_cast<f64>(rise) * (x + 1);
                        edgeWeight += rise;
                    }
                }
                REQUIRE(edgeWeight > 0.0);
                edges.push_back(static_cast<f32>(edgeSum / edgeWeight));

                // The star's centroid over its window, above the mean of the window's border ring.
                f64 ring = 0.0;
                u32 ringCount = 0;
                for (i32 dy = -StarRadius; dy <= StarRadius; ++dy)
                {
                    for (i32 dx = -StarRadius; dx <= StarRadius; ++dx)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) == StarRadius)
                        {
                            ring += frame.Luminance[Index(star.x + dx, star.y + dy)];
                            ++ringCount;
                        }
                    }
                }
                const f32 background = static_cast<f32>(ring / ringCount);
                f64 sx = 0.0;
                f64 sy = 0.0;
                f64 sw = 0.0;
                for (i32 dy = -StarRadius; dy <= StarRadius; ++dy)
                {
                    for (i32 dx = -StarRadius; dx <= StarRadius; ++dx)
                    {
                        const f32 weight = std::max(
                            frame.Luminance[Index(star.x + dx, star.y + dy)] - background, 0.0f);
                        sx += static_cast<f64>(weight) * (star.x + dx + 0.5);
                        sy += static_cast<f64>(weight) * (star.y + dy + 0.5);
                        sw += weight;
                    }
                }
                REQUIRE(sw > 0.0);
                starsX.push_back(static_cast<f32>(sx / sw));
                starsY.push_back(static_cast<f32>(sy / sw));
            }

            result.EdgeWander = Wander(edges);
            result.StarWander = std::max(Wander(starsX), Wander(starsY));
            return result;
        }

        Test::GpuFixture& Gpu;
        AssetManager Assets;
        path Archive;
        Unique<Scene> World;
        CameraView Camera;
    };

    // A converged resolve over a static view changes the open sky and the cube's interior by under
    // half the sky's per-output-pixel step (about 6e-3 at half scale's gradient), where a resolve
    // that lets the image follow the jitter moves the sky by most of a step.
    constexpr f32 MaxFrameDelta = 3.0e-3f;
    // A smooth feature (the star) holds its centroid to a small fraction of a pixel.
    constexpr f32 MaxStarWander = 0.05f;
    // A hard silhouette is point-sampled every frame, so its coverage flips with the jitter and the
    // exponential history (a tenth of each frame) ripples by about a tenth of that flip; the edge
    // keeps its place to within that ripple rather than following the jitter.
    constexpr f32 MaxEdgeWander = 0.1f;

    // How still a converged resolve must hold.
    struct StillnessBounds
    {
        f32 FrameDelta = MaxFrameDelta;
        f32 StarWander = MaxStarWander;
        f32 EdgeWander = MaxEdgeWander;
    };

    // MetalFX's own reconstruction leaves a residual the engine does not control: measured at
    // landing, up to 6e-3 in the cube and 0.07 px on the star. A jitter the scaler is told wrongly
    // and so leaves in the image moves the edge by a pixel and the star by more, so these bounds
    // sit an order of magnitude under the failure they exist to catch.
    constexpr StillnessBounds MetalFxBounds{.FrameDelta = 1.5e-2f, .StarWander = 0.15f};

    void CheckStill(const Stability& s, const StillnessBounds& bounds = {})
    {
        CAPTURE(s.CubeDelta);
        CAPTURE(s.SkyDelta);
        CAPTURE(s.EdgeWander);
        CAPTURE(s.StarWander);
        REQUIRE(s.CubePixels > 50);
        REQUIRE(s.SkyPixels > 500);
        CHECK(s.CubeDelta < bounds.FrameDelta);
        CHECK(s.SkyDelta < bounds.FrameDelta);
        CHECK(s.EdgeWander < bounds.EdgeWander);
        CHECK(s.StarWander < bounds.StarWander);
    }

    void CheckStatic(StabilityScene& scene, const AntiAliasingMode mode,
                     const StillnessBounds& bounds = {})
    {
        CHECK(scene.MaxMotion(mode) == 0.0f);
        for (const f32 renderScale : {1.0f, 0.5f})
        {
            CAPTURE(renderScale);
            CheckStill(scene.Measure(mode, renderScale), bounds);
        }
    }

    // The cube's left silhouette along the middle row band, as the luminance-rise-weighted mean of
    // the pixel boundaries around the first cube pixel.
    f32 LeftEdge(const HdrFrame& frame)
    {
        const i32 w = static_cast<i32>(Extent.x);
        const i32 h = static_cast<i32>(Extent.y);
        i32 edgeX = -1;
        for (i32 x = 0; x < w && edgeX < 0; ++x)
        {
            if (frame.Luminance[Index(x, h / 2)] > CubeThreshold)
            {
                edgeX = x;
            }
        }
        REQUIRE(edgeX > 6);
        f64 sum = 0.0;
        f64 weight = 0.0;
        for (i32 y = h / 3; y <= 2 * h / 3; ++y)
        {
            for (i32 x = edgeX - 5; x < edgeX + 4; ++x)
            {
                const f32 rise =
                    std::max(frame.Luminance[Index(x + 1, y)] - frame.Luminance[Index(x, y)], 0.0f);
                sum += static_cast<f64>(rise) * (x + 1);
                weight += rise;
            }
        }
        REQUIRE(weight > 0.0);
        return static_cast<f32>(sum / weight);
    }

    // The mean luminance of the sky in the output's bottom-right corner, the region a promotion
    // reading the wrong part of its source would show something else in.
    f32 CornerSky(const HdrFrame& frame)
    {
        f64 sum = 0.0;
        u32 count = 0;
        for (u32 y = Extent.y - 10; y < Extent.y - BorderBand; ++y)
        {
            for (u32 x = Extent.x - 10; x < Extent.x - BorderBand; ++x)
            {
                sum += frame.Luminance[Index(static_cast<i32>(x), static_cast<i32>(y))];
                ++count;
            }
        }
        return static_cast<f32>(sum / count);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "taa stability: a static view converges to a still image under TAA")
{
    StabilityScene scene(*this);
    CheckStatic(scene, AntiAliasingMode::TAA);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "taa stability: a static view converges to a still image under TAAU")
{
    StabilityScene scene(*this);
    CheckStatic(scene, AntiAliasingMode::TAAU);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "taa stability: a static view converges to a still image with depth of field after the resolve")
{
    // Under a composited depth of field the resolve writes the chain's scene intermediate and the
    // composite writes the HDR target later in the frame, so the history copy reads the resolve's
    // own target; the image the chain composites still converges like the plain resolve's.
    StabilityScene scene(*this);
    CheckStill(scene.Measure(AntiAliasingMode::TAA, 1.0f, true));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "taa stability: a static view converges to a still image under MetalFX temporal")
{
    StabilityScene scene(*this);
    // The renderer keeps the choice exactly where the device readout says the scaler runs (at
    // these allocations, well inside its scale range), and runs TAAU elsewhere.
    const bool supported = Context.GetDeviceCapabilities().MetalFxTemporal;
    CHECK((scene.Resolved({.AntiAliasing = AntiAliasingMode::MetalFXTemporal}).AntiAliasing ==
           AntiAliasingMode::MetalFXTemporal) == supported);
    if (!supported)
    {
        MESSAGE("MetalFX temporal scaling is unavailable on this device; nothing to measure");
        return;
    }
    CheckStatic(scene, AntiAliasingMode::MetalFXTemporal, MetalFxBounds);
    // With a composited depth of field the scaler reconstructs the chain's intermediate at the
    // render allocation instead of promoting.
    CheckStill(scene.Measure(AntiAliasingMode::MetalFXTemporal, 1.0f, true), MetalFxBounds);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "upscale: a MetalFX spatial promotion places a half-scale render where native lies")
{
    StabilityScene scene(*this);
    const SceneRendererSettings settings{.Mode = DebugView::Final,
                                         .Bloom = false,
                                         .Upscale = UpscaleFilter::MetalFXSpatial,
                                         .Shadows = false,
                                         .PunctualShadows = false,
                                         .AO = false};
    const bool supported = Context.GetDeviceCapabilities().MetalFxSpatial;
    CHECK((scene.Resolved(settings).Upscale == UpscaleFilter::MetalFXSpatial) == supported);
    if (!supported)
    {
        MESSAGE("MetalFX spatial scaling is unavailable on this device; nothing to measure");
        return;
    }

    const HdrFrame native = scene.Output(settings, 1.0f);
    const HdrFrame half = scene.Output(settings, 0.5f);
    // A half-scale render places an edge only to a render pixel, two output pixels wide, and an
    // edge-adaptive filter sharpens it within that pixel (measured at landing: 0.7 output px from
    // native, against 0.4 for the bilinear tap). A promotion reading the wrong part of its source
    // moves it by many pixels; it must stay within half a render pixel.
    CHECK(std::abs(LeftEdge(half) - LeftEdge(native)) < 1.0f);
    // The sky is a smooth gradient, so the upscale reproduces it closely wherever it read the
    // right part of the source.
    CHECK(CornerSky(half) == doctest::Approx(CornerSky(native)).epsilon(0.05));
}
