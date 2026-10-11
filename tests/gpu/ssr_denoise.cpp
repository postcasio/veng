// The stochastic reflection chain's denoiser: one noisy ray per traced pixel reprojected, filtered
// across the surface it stands on and resolved against its history. Every case reads the chain's
// denoised target:
//
//  - a still view converges with anti-aliasing off: the denoiser is a reflection's only history;
//  - under the projection's jitter a normal-mapped floor's reflection holds still, though each
//    phase turns every ray;
//  - a reflection whose source goes is gone the next frame, and one whose source returns builds
//    back up frame by frame;
//  - the spatial filter stops at a silhouette, from either side;
//  - a mirror is not filtered spatially: its reflected edge is as sharp as the trace's.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD.

#include <algorithm>
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
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/TaaJitter.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // forward_lighting_pack's opaque material: BaseColor, Roughness, Metallic, Occlusion, untextured.
    constexpr AssetId OpaqueMaterial{0x3AEA87F2E94072A6ULL};
    // white_plane_pack's normal-mapped metal (normal_mapped.frag): its NormalMap is a field of
    // independent per-texel tilts, repeated UVScale times across the plane.
    constexpr AssetId NormalMappedInstanceId{0xBCAE43829219AE51ULL};
    // The same pack's brick material, the parent of the emissive bars and the dark wall.
    constexpr AssetId BrickMaterial{0x000000000000232BULL};

    constexpr uvec2 Extent{128, 128};
    constexpr usize Pixels = static_cast<usize>(Extent.x) * Extent.y;

    // The iteration budget the intersection's design calls for.
    constexpr u32 TraversalBudget = 128;

    path MountPack(AssetManager& assets, const char* archiveName, const char* pack)
    {
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, path(GPU_GBUFFER_FIXTURE_DIR) / pack,
                                              outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                                              {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        return outArchive;
    }

    AssetHandle<MaterialInstance> MakeMaterial(Context& context, AssetManager& assets,
                                               const char* name, const vec3& colour,
                                               const f32 roughness, const f32 metallic)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(OpaqueMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = name, .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColor", vec4(colour, 1.0f));
        mat.SetParam("Roughness", roughness);
        mat.SetParam("Metallic", metallic);
        return instance;
    }

    AssetHandle<MaterialInstance> MakeBrick(Context& context, AssetManager& assets,
                                            const char* name, const vec3& colour,
                                            const vec3& emissive)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(BrickMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = name, .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColorFactor", vec4(colour, 1.0f));
        mat.SetParam("EmissiveFactor", vec4(emissive, 0.0f));
        return instance;
    }

    // A plane standing upright, facing +Z.
    quat Upright()
    {
        return glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
    }

    struct Built
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        vector<Entity> Bars;
        CameraView Camera;
    };

    Entity Place(Built& built, const AssetManager& assets, const Ref<Mesh>& mesh,
                 const vec3& position, const quat& rotation = quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        built.Meshes.push_back(mesh);
        const Entity entity = built.World->CreateEntity();
        auto& transform = built.World->Add<Transform>(entity);
        transform.Position = position;
        transform.Rotation = rotation;
        built.World->Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
        return entity;
    }

    // A metal floor of white_plane_pack's normal-mapped material reflecting five emissive bars
    // standing on it in front of a dark wall, with no light and no ambient term: everything the
    // floor shows is reflection. A normal strength of 0 makes the floor flat.
    Built BuildBarsScene(Context& context, AssetManager& assets, TypeRegistry& types,
                         const f32 normalStrength, const f32 roughness)
    {
        Built built{.World = Scene::Create(types)};
        const AssetResult<AssetHandle<MaterialInstance>> floor =
            assets.LoadSync<MaterialInstance>(NormalMappedInstanceId);
        REQUIRE(floor.has_value());
        auto& floorMaterial = const_cast<MaterialInstance&>(*floor->Get());
        floorMaterial.SetParam("MetallicFactor", 1.0f);
        floorMaterial.SetParam("RoughnessFactor", roughness);
        floorMaterial.SetParam("UVScale", 0.5f);
        floorMaterial.SetParam("NormalStrength", normalStrength);

        Place(built, assets,
              Mesh::BuildSync(context, Primitives::Plane(vec2(8.0f), uvec2(1), *floor), "Floor"),
              vec3(0.0f));
        Place(built, assets,
              Mesh::BuildSync(context,
                              Primitives::Plane(
                                  vec2(6.0f, 2.0f), uvec2(1),
                                  MakeBrick(context, assets, "Dark Wall", vec3(0.02f), vec3(0.0f))),
                              "Dark Wall"),
              vec3(0.0f, 1.0f, -2.0f), Upright());
        const AssetHandle<MaterialInstance> bar =
            MakeBrick(context, assets, "Bar", vec3(0.0f), vec3(2.0f));
        for (const f32 x : {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f})
        {
            built.Bars.push_back(
                Place(built, assets,
                      Mesh::BuildSync(context, Primitives::Plane(vec2(0.12f, 0.8f), uvec2(1), bar),
                                      "Bar"),
                      vec3(x, 0.4f, -1.9f), Upright()));
        }

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
        built.Camera.SetView(vec3(0.0f, 0.6f, 1.5f), vec3(0.0f, 0.15f, -2.0f),
                             vec3(0.0f, 1.0f, 0.0f));
        return built;
    }

    SceneRendererSettings
    StochasticSettings(const AntiAliasingMode mode,
                       const SceneRendererSettings::SsrSamplesPerQuad density =
                           SceneRendererSettings::SsrSamplesPerQuad::One)
    {
        SceneRendererSettings settings{
            .Mode = DebugView::Final, .Bloom = false, .Shadows = false, .SSR = true, .AO = false};
        settings.PunctualShadows = false;
        settings.AntiAliasing = mode;
        settings.SsrMethod = SceneRendererSettings::SsrTechnique::Stochastic;
        settings.SsrDensity = density;
        return settings;
    }

    vector<vec4> DecodeRgba16f(const vector<u8>& bytes)
    {
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        vector<vec4> out(Pixels);
        for (usize i = 0; i < out.size(); ++i)
        {
            out[i] = vec4(
                glm::unpackHalf1x16(halves[(i * 4) + 0]), glm::unpackHalf1x16(halves[(i * 4) + 1]),
                glm::unpackHalf1x16(halves[(i * 4) + 2]), glm::unpackHalf1x16(halves[(i * 4) + 3]));
        }
        return out;
    }

    f32 Luma(const vec3& c)
    {
        return glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
    }

    // One frame's reflection, premultiplied by its confidence: the denoiser's output and the
    // intersection's raw trace it was made from, both in the frame's jittered raster, and the lit
    // scene (the frame composites no reflection).
    struct Frame
    {
        vector<vec4> Denoised;
        vector<vec4> Raw;
        vector<vec4> Lit;
        // The frame's projection jitter in pixels: where the scene at a pixel's centre stands in
        // the raster.
        vec2 JitterPixels{0.0f};

        // The denoised luminance at the scene point pixel (x, y)'s centre shows, read bilinearly
        // from the jittered raster as a temporal resolve reads it.
        [[nodiscard]] f32 DenoisedLuma(const u32 x, const u32 y) const
        {
            const vec2 position = vec2(x, y) + JitterPixels;
            const ivec2 base = ivec2(glm::floor(position));
            const vec2 f = position - vec2(base);
            const auto At = [&](const i32 dx, const i32 dy)
            {
                const ivec2 p =
                    glm::clamp(base + ivec2(dx, dy), ivec2(0), ivec2(Extent) - ivec2(1));
                return Luma(vec3(Denoised[(static_cast<usize>(p.y) * Extent.x) + p.x]));
            };
            return glm::mix(glm::mix(At(0, 0), At(1, 0), f.x), glm::mix(At(0, 1), At(1, 1), f.x),
                            f.y);
        }

        [[nodiscard]] f32 RawLuma(const u32 x, const u32 y) const
        {
            return Luma(vec3(Raw[(static_cast<usize>(y) * Extent.x) + x]));
        }

        [[nodiscard]] f32 LitLuma(const u32 x, const u32 y) const
        {
            return Luma(vec3(Lit[(static_cast<usize>(y) * Extent.x) + x]));
        }
    };

    // A stochastic-chain renderer and the frames it has executed, whose count picks each frame's
    // projection jitter.
    struct Probe
    {
        Unique<SceneRenderer> Renderer;
        AntiAliasingMode Mode = AntiAliasingMode::None;
        u64 Frames = 0;

        Probe(Context& context, AssetManager& assets, const AntiAliasingMode mode,
              const SceneRendererSettings::SsrSamplesPerQuad density =
                  SceneRendererSettings::SsrSamplesPerQuad::One)
            : Renderer(SceneRenderer::Create({
                  .Context = context,
                  .Assets = assets,
                  .OutputFormat = context.GetOutputFormat(),
                  .Extent = Extent,
                  .Settings = StochasticSettings(mode, density),
              })),
              Mode(mode)
        {
        }

        Frame Render(const Context& context, const Scene& scene, const CameraView& camera)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Renderer->Execute(cmd, Renderer::SceneView{.World = scene,
                                                               .Camera = camera,
                                                               .Delta = 0.0f,
                                                               .AmbientFloor = vec3(0.0f),
                                                               .SsrMaxSteps = TraversalBudget});
                });
            const SsrTraceTargets targets = Renderer->GetSsrTraceTargets();
            REQUIRE(targets.Denoised != nullptr);
            Frame frame{.Denoised = DecodeRgba16f(targets.Denoised->GetImage()->Download()),
                        .Raw = DecodeRgba16f(targets.Radiance->GetImage()->Download()),
                        .Lit = DecodeRgba16f(Renderer->GetHdrView()->GetImage()->Download())};
            if (Mode == AntiAliasingMode::TAA)
            {
                frame.JitterPixels =
                    TaaJitterNdc(TaaJitterOffset(Frames), Extent) * 0.5f * vec2(Extent);
            }
            ++Frames;
            return frame;
        }
    };

    // The floor rows the bars' reflection falls on: below the lowest bar pixel the scene shows,
    // clear of the image border.
    struct FloorBand
    {
        u32 FirstRow = 0;
        u32 LastRow = 0;
    };

    FloorBand LocateFloor(const Frame& frame)
    {
        u32 contact = 0;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                contact = frame.LitLuma(x, y) > 0.5f ? std::max(contact, y) : contact;
            }
        }
        return {.FirstRow = contact + 3, .LastRow = Extent.y - 3};
    }

    // The band's mean denoised luminance.
    f64 BandMean(const Frame& frame, const FloorBand& band)
    {
        f64 sum = 0.0;
        for (u32 y = band.FirstRow; y <= band.LastRow; ++y)
        {
            for (u32 x = 2; x < Extent.x - 2; ++x)
            {
                sum += frame.DenoisedLuma(x, y);
            }
        }
        return sum / static_cast<f64>((band.LastRow - band.FirstRow + 1) * (Extent.x - 4));
    }

    // The band's mean absolute difference in denoised luminance between two frames.
    f64 BandMeanDelta(const Frame& a, const Frame& b, const FloorBand& band)
    {
        f64 sum = 0.0;
        for (u32 y = band.FirstRow; y <= band.LastRow; ++y)
        {
            for (u32 x = 2; x < Extent.x - 2; ++x)
            {
                sum += std::abs(a.DenoisedLuma(x, y) - b.DenoisedLuma(x, y));
            }
        }
        return sum / static_cast<f64>((band.LastRow - band.FirstRow + 1) * (Extent.x - 4));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr denoise: a still view converges with anti-aliasing off")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_denoise_still.vengpack", "forward_lighting_pack.json");

    // A glossy floor reflecting three lit panels of different colours, lit from behind the camera.
    Built built{.World = Scene::Create(Types)};
    Place(built, assets,
          Mesh::BuildSync(
              Context,
              Primitives::Plane(vec2(8.0f), uvec2(1),
                                MakeMaterial(Context, assets, "Floor", vec3(1.0f), 0.3f, 1.0f)),
              "Floor"),
          vec3(0.0f));
    for (const auto& [x, colour] :
         {std::pair{-0.6f, vec3(0.9f, 0.1f, 0.1f)}, std::pair{0.0f, vec3(0.1f, 0.9f, 0.1f)},
          std::pair{0.6f, vec3(0.2f, 0.3f, 0.9f)}})
    {
        Place(built, assets,
              Mesh::BuildSync(
                  Context,
                  Primitives::Plane(vec2(0.5f, 0.6f), uvec2(1),
                                    MakeMaterial(Context, assets, "Panel", colour, 1.0f, 0.0f)),
                  "Panel"),
              vec3(x, 0.3f, -1.5f), Upright());
    }
    const Entity light = built.World->CreateEntity();
    built.World->Add<Light>(light) = Light{
        .Type = LightType::Directional,
        .Direction = glm::normalize(vec3(0.0f, -0.3f, -1.0f)),
        .Color = vec3(1.0f),
        .Intensity = 8.0f / LuminousAnchor,
    };
    built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
    built.Camera.SetView(vec3(0.0f, 0.6f, 1.5f), vec3(0.0f, 0.15f, -2.0f), vec3(0.0f, 1.0f, 0.0f));

    Probe probe(Context, assets, AntiAliasingMode::None);
    Frame previous;
    for (u32 frame = 0; frame < 32; ++frame)
    {
        previous = probe.Render(Context, *built.World, built.Camera);
    }

    // Where the floor reflects the panels: confidently reflected and bright.
    vector<usize> reflecting;
    f64 level = 0.0;
    for (usize i = 0; i < Pixels; ++i)
    {
        if (previous.Denoised[i].a > 0.5f && Luma(vec3(previous.Denoised[i])) > 0.02f)
        {
            reflecting.push_back(i);
            level += Luma(vec3(previous.Denoised[i]));
        }
    }
    REQUIRE(reflecting.size() > 256);
    level /= static_cast<f64>(reflecting.size());

    const auto MeanOf = [&](const Frame& frame)
    {
        f64 sum = 0.0;
        for (const usize i : reflecting)
        {
            sum += Luma(vec3(frame.Denoised[i]));
        }
        return sum / static_cast<f64>(reflecting.size());
    };
    f64 worstLevel = 0.0;
    f64 worstPixel = 0.0;
    for (u32 frame = 0; frame < 16; ++frame)
    {
        Frame current = probe.Render(Context, *built.World, built.Camera);
        f64 change = 0.0;
        for (const usize i : reflecting)
        {
            change += std::abs(Luma(vec3(current.Denoised[i])) - Luma(vec3(previous.Denoised[i])));
        }
        worstPixel = std::max(worstPixel, change / static_cast<f64>(reflecting.size()) / level);
        worstLevel = std::max(worstLevel, std::abs(MeanOf(current) - MeanOf(previous)) / level);
        previous = std::move(current);
    }
    CAPTURE(level);
    CAPTURE(worstLevel);
    CAPTURE(worstPixel);
    // Every frame's rays are new, so each pixel's raw reflection changes by about half its level.
    // The reflection's level settles under half a percent a frame (0.44 % at landing); a pixel keeps
    // a little more flicker (0.91 %), which the 3x3 clip trades for a sharp, prompt history.
    CHECK(worstLevel < 0.005);
    CHECK(worstPixel < 0.015);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr denoise: a normal-mapped floor's reflection holds still under the jitter")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_denoise_footprint.vengpack", "white_plane_pack.json");
    const Built built = BuildBarsScene(Context, assets, Types, 0.3f, 0.05f);
    Probe probe(Context, assets, AntiAliasingMode::TAA);

    Frame previous;
    for (u32 frame = 0; frame < 9 * TaaJitterSampleCount; ++frame)
    {
        previous = probe.Render(Context, *built.World, built.Camera);
    }
    const FloorBand band = LocateFloor(previous);
    REQUIRE(band.LastRow > band.FirstRow + 16);
    const f64 level = BandMean(previous, band);
    REQUIRE(level > 0.01);

    // Each phase samples the normal map somewhere else and so turns every ray. The denoised term
    // stands in the jittered raster, so it is compared where the scene stands, as the frame's
    // resolve reads it.
    f64 deviation = 0.0;
    for (u32 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        Frame current = probe.Render(Context, *built.World, built.Camera);
        deviation += BandMeanDelta(current, previous, band) / level / TaaJitterSampleCount;
        previous = std::move(current);
    }
    CAPTURE(level);
    CAPTURE(deviation);
    CHECK(deviation < 0.05);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr denoise: a reflection follows its source going and returning")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_denoise_reset.vengpack", "white_plane_pack.json");

    for (const f32 strength : {0.0f, 0.3f})
    {
        CAPTURE(strength);
        Built built = BuildBarsScene(Context, assets, Types, strength, 0.05f);
        Probe probe(Context, assets, AntiAliasingMode::None);
        Frame converged;
        for (u32 frame = 0; frame < 48; ++frame)
        {
            converged = probe.Render(Context, *built.World, built.Camera);
        }
        const FloorBand band = LocateFloor(converged);
        REQUIRE(band.LastRow > band.FirstRow + 16);
        const f64 level = BandMean(converged, band);
        REQUIRE(level > 0.01);

        // Dropped under the floor, the bars reflect nowhere: the floor now reflects a black wall.
        const auto shift = [&](const f32 dy)
        {
            for (const Entity bar : built.Bars)
            {
                built.World->Get<Transform>(bar).Position.y += dy;
            }
        };
        constexpr u32 Frames = 10;
        vector<f64> gone;
        shift(-100.0f);
        for (u32 frame = 0; frame < Frames; ++frame)
        {
            gone.push_back(BandMean(probe.Render(Context, *built.World, built.Camera), band) /
                           level);
        }
        vector<f64> back;
        shift(100.0f);
        for (u32 frame = 0; frame < Frames; ++frame)
        {
            back.push_back(BandMean(probe.Render(Context, *built.World, built.Camera), band) /
                           level);
        }
        f64 goneMost = 0.0;
        f64 backFall = 0.0;
        for (u32 frame = 0; frame < Frames; ++frame)
        {
            goneMost = std::max(goneMost, gone[frame]);
            backFall = frame > 0 ? std::max(backFall, back[frame - 1] - back[frame]) : backFall;
        }
        CAPTURE(goneMost);
        CAPTURE(backFall);
        CAPTURE(back.back());
        // The history is clipped to this frame's filtered neighbourhood, which holds nothing bright
        // once the source has gone, so the reflection is gone the first frame after (at landing
        // under 0.3 % of its level). A returning source builds back every frame; the gaps between
        // the bars keep the neighbourhood's lower bound dark, so the dark history is clipped only
        // from above and returns at its blend's pace (44 % and 47 % ten frames on at landing).
        CHECK(goneMost < 0.05);
        CHECK(backFall < 0.005);
        CHECK(back.back() > 0.3);
    }

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr denoise: the spatial filter stops at a silhouette, from either side")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_denoise_silhouette.vengpack", "forward_lighting_pack.json");

    // A glossy floor reflecting a bright matte backdrop, and in front of it a panel of the same
    // glossy metal facing the camera, whose reflected rays head back toward the camera and meet
    // nothing on screen.
    constexpr f32 Roughness = 0.05f;
    Built built{.World = Scene::Create(Types)};
    Place(built, assets,
          Mesh::BuildSync(Context,
                          Primitives::Plane(
                              vec2(40.0f), uvec2(1),
                              MakeMaterial(Context, assets, "Floor", vec3(1.0f), Roughness, 1.0f)),
                          "Floor"),
          vec3(0.0f));
    Place(built, assets,
          Mesh::BuildSync(
              Context,
              Primitives::Plane(vec2(12.0f, 6.0f), uvec2(1),
                                MakeMaterial(Context, assets, "Backdrop", vec3(0.9f), 1.0f, 0.0f)),
              "Backdrop"),
          vec3(0.0f, 3.0f, -5.0f), Upright());
    Place(built, assets,
          Mesh::BuildSync(Context,
                          Primitives::Plane(
                              vec2(0.6f, 0.8f), uvec2(1),
                              MakeMaterial(Context, assets, "Panel", vec3(1.0f), Roughness, 1.0f)),
                          "Panel"),
          vec3(0.0f, 0.4f, -0.5f), Upright());
    const Entity light = built.World->CreateEntity();
    built.World->Add<Light>(light) = Light{
        .Type = LightType::Directional,
        .Direction = glm::normalize(vec3(0.0f, -0.4f, -1.0f)),
        .Color = vec3(1.0f),
        .Intensity = 8.0f / LuminousAnchor,
    };
    built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
    built.Camera.SetView(vec3(0.0f, 0.9f, 2.2f), vec3(0.0f, 0.1f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

    // Every pixel traces its own ray, so no quad's copy carries a ray across the silhouette and the
    // filter alone is judged there.
    Probe probe(Context, assets, AntiAliasingMode::None,
                SceneRendererSettings::SsrSamplesPerQuad::Four);
    Frame frame;
    vector<vec4> traced(Pixels, vec4(0.0f));
    constexpr u32 Frames = 32;
    for (u32 index = 0; index < Frames; ++index)
    {
        frame = probe.Render(Context, *built.World, built.Camera);
        for (usize i = 0; i < Pixels; ++i)
        {
            traced[i] += frame.Raw[i] / static_cast<f32>(Frames);
        }
    }
    const auto TracedLuma = [&](const u32 x, const u32 y)
    { return Luma(vec3(traced[(static_cast<usize>(y) * Extent.x) + x])); };

    // Which surface each pixel is, from the normals' debug arm (n * 0.5 + 0.5).
    enum class Surface : u8
    {
        Other,
        Floor,
        Panel,
    };
    vector<Surface> surfaces(Pixels);
    {
        const Unique<SceneRenderer> normals = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Normal},
        });
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                normals->Execute(cmd, Renderer::SceneView{.World = *built.World,
                                                          .Camera = built.Camera,
                                                          .Delta = 0.0f});
            });
        const vector<vec4> encoded = DecodeRgba16f(normals->GetOutput()->GetImage()->Download());
        for (usize i = 0; i < Pixels; ++i)
        {
            const vec3 normal = vec3(encoded[i]) * 2.0f - 1.0f;
            surfaces[i] = normal.y > 0.9f   ? Surface::Floor
                          : normal.z > 0.9f ? Surface::Panel
                                            : Surface::Other;
        }
    }
    const auto SurfaceAt = [&](const u32 x, const u32 y)
    { return surfaces[(static_cast<usize>(y) * Extent.x) + x]; };

    // The panel's lower half, where floor flanks it on both sides, clear of its top corners and of
    // the crease where it meets the floor.
    u32 top = Extent.y;
    u32 bottom = 0;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        if (SurfaceAt(0, y) == Surface::Floor && SurfaceAt(Extent.x / 2, y) == Surface::Panel)
        {
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    REQUIRE(bottom > top + 12);

    // Within the filter's reach of the edge: what the panel takes of the floor's reflection, and
    // what the floor keeps of the reflection its own rays brought it there. Its rays nearest the
    // edge pass behind the panel on screen and miss, so the floor redistributes that loss across
    // its own surface; what crosses the edge would show as the floor's reflection lost or the
    // panel's gained.
    constexpr u32 Reach = 4;
    f32 panelMost = 0.0f;
    f64 floorDenoised = 0.0;
    f64 floorTraced = 0.0;
    u32 rows = 0;
    for (u32 y = (top + bottom) / 2; y + 3 <= bottom; ++y)
    {
        u32 left = Extent.x / 2;
        while (SurfaceAt(left - 1, y) == Surface::Panel)
        {
            --left;
        }
        u32 right = Extent.x / 2;
        while (SurfaceAt(right + 1, y) == Surface::Panel)
        {
            ++right;
        }
        REQUIRE(left > 16);
        REQUIRE(right + 16 < Extent.x);
        // The floor's reflection here, well clear of the panel.
        f32 plateau = 0.0f;
        for (u32 k = 8; k < 16; ++k)
        {
            plateau += (frame.DenoisedLuma(left - k, y) + frame.DenoisedLuma(right + k, y)) / 16.0f;
        }
        REQUIRE(plateau > 0.0f);
        for (u32 k = 0; k < Reach; ++k)
        {
            panelMost = std::max({panelMost, frame.DenoisedLuma(left + k, y) / plateau,
                                  frame.DenoisedLuma(right - k, y) / plateau});
            floorDenoised +=
                frame.DenoisedLuma(left - 1 - k, y) + frame.DenoisedLuma(right + 1 + k, y);
            floorTraced += TracedLuma(left - 1 - k, y) + TracedLuma(right + 1 + k, y);
        }
        ++rows;
    }
    REQUIRE(rows >= 4);
    const f64 floorKept = floorDenoised / floorTraced;
    CAPTURE(panelMost);
    CAPTURE(floorKept);
    // The spatial filter's normal weight stops at the edge and the temporal clip takes only this
    // frame's neighbourhood, so the panel takes none of the floor's level (under 0.01 % at landing).
    CHECK(panelMost < 0.01f);
    CHECK(floorKept > 0.9);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr denoise: a mirror's reflected edge is as sharp as the trace's")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive =
        MountPack(assets, "veng_gpu_ssr_denoise_mirror.vengpack", "white_plane_pack.json");
    // A flat floor whose GGX alpha stores as zero in the R8 alpha target: a mirror.
    const Built built = BuildBarsScene(Context, assets, Types, 0.0f, 0.005f);
    Probe probe(Context, assets, AntiAliasingMode::None);
    Frame frame;
    for (u32 index = 0; index < 32; ++index)
    {
        frame = probe.Render(Context, *built.World, built.Camera);
    }
    const FloorBand band = LocateFloor(frame);
    REQUIRE(band.LastRow > band.FirstRow + 16);

    // A row a few pixels into the bars' reflection, and the bar reflected nearest its middle.
    const u32 y = band.FirstRow + 4;
    u32 peak = Extent.x / 2;
    for (u32 x = Extent.x / 2 - 8; x < Extent.x / 2 + 8; ++x)
    {
        peak = frame.RawLuma(x, y) > frame.RawLuma(peak, y) ? x : peak;
    }
    const f32 bright = frame.RawLuma(peak, y);
    REQUIRE(bright > 0.5f);

    // The 10-90 % width of the bar's left edge, between the gap beside it and its peak.
    const auto EdgeWidth = [&](const auto& luma)
    {
        const f32 high = luma(peak);
        u32 edge = peak;
        while (edge > 4 && luma(edge) >= 0.5f * high)
        {
            --edge;
        }
        const u32 start = edge - 4;
        f32 low = luma(start);
        for (u32 x = start; x <= edge; ++x)
        {
            low = std::min(low, luma(x));
        }
        const auto Crossing = [&](const f32 level)
        {
            u32 x = start;
            while (x < peak && luma(x + 1) < level)
            {
                ++x;
            }
            return static_cast<f32>(x) +
                   std::clamp((level - luma(x)) / std::max(luma(x + 1) - luma(x), 1e-6f), 0.0f,
                              1.0f);
        };
        return Crossing(low + (0.9f * (high - low))) - Crossing(low + (0.1f * (high - low)));
    };
    const f32 traced = EdgeWidth([&](const u32 x) { return frame.RawLuma(x, y); });
    const f32 denoised = EdgeWidth([&](const u32 x) { return frame.DenoisedLuma(x, y); });
    CAPTURE(traced);
    CAPTURE(denoised);
    CHECK(std::abs(denoised - traced) < 0.25f);

    std::filesystem::remove(archive);
}
