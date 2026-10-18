// The particle chain on a real device, at a pool of 64: a particle drawn from the closed form lands
// where the CPU twin puts it, a burst spawns exactly its count, a burst past the pool is clamped and
// counted, death and an early retire empty the pool and idle the chain, alpha particles sort back to
// front whichever spawned first, a fed particle draws where it was placed, each particle plays the
// flipbook variant its key picks, and a streak lights the span between its tail and its head. A
// white premultiplied flipbook makes a billboard's pixel its tint.

#include <array>
#include <vector>

#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/ParticleEffect.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/ParticleEffectSystem.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "Renderer/ParticleMath.h"

// glm's constants after Veng.h, which configures glm first.
#include <glm/gtc/constants.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};
    constexpr u32 Capacity = 64;

    // A 2x2 grid of 4x4 fully covered frames of one premultiplied colour: sampled anywhere it reads
    // (r, g, b, 1).
    AssetHandle<Flipbook> SolidFlipbook(Context& context, const u8 r, const u8 g, const u8 b)
    {
        std::array<u8, 8 * 8 * 4> pixels{};
        for (usize texel = 0; texel < 64; ++texel)
        {
            pixels[texel * 4] = r;
            pixels[texel * 4 + 1] = g;
            pixels[texel * 4 + 2] = b;
            pixels[texel * 4 + 3] = 255;
        }
        const Ref<Texture> atlas = Veng::Detail::BuildAssetSync(
            context, TextureData{
                         .Name = "Particle Atlas",
                         .Extent = {8, 8},
                         .Format = Format::RGBA8Unorm,
                         .Pixels = pixels,
                         .Sampler = {.AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge},
                     });
        return AssetManager::Adopt(Flipbook::Create(FlipbookInfo{
            .Name = "White",
            .Atlas = atlas,
            .Columns = 2,
            .Rows = 2,
            .FrameSize = {4, 4},
            .Clip = {.FrameCount = 4, .Fps = 10.0f, .Loop = true},
            .AlphaMode = FlipbookAlpha::Premultiplied,
        }));
    }

    // White: a billboard's pixel is its tint.
    AssetHandle<Flipbook> WhiteFlipbook(Context& context)
    {
        return SolidFlipbook(context, 255, 255, 255);
    }

    // A one-frame six-way flipbook lit only through lightA's +X map, fully covered, cold.
    AssetHandle<Flipbook> SixWayFlipbook(Context& context)
    {
        const auto solid = [&](const char* name, const vec4 value)
        {
            std::array<u16, 4 * 4 * 4> halves{};
            for (usize texel = 0; texel < 16; ++texel)
            {
                for (usize c = 0; c < 4; ++c)
                {
                    halves[texel * 4 + c] = glm::packHalf1x16(value[static_cast<int>(c)]);
                }
            }
            return Veng::Detail::BuildAssetSync(
                context, TextureData{
                             .Name = name,
                             .Extent = {4, 4},
                             .Format = Format::RGBA16Sfloat,
                             .Pixels = std::span(reinterpret_cast<const u8*>(halves.data()),
                                                 halves.size() * sizeof(u16)),
                             .Sampler = {.AddressModeU = AddressMode::ClampToEdge,
                                         .AddressModeV = AddressMode::ClampToEdge,
                                         .AddressModeW = AddressMode::ClampToEdge},
                         });
        };
        return AssetManager::Adopt(Flipbook::Create(FlipbookInfo{
            .Name = "Six-Way",
            .Atlas = solid("Light A", vec4(1.0f, 0.0f, 0.0f, 1.0f)),
            .FrameSize = {4, 4},
            .Clip = {.FrameCount = 1, .Fps = 10.0f, .Loop = true},
            .AlphaMode = FlipbookAlpha::Premultiplied,
            .Shading = FlipbookShading::SixWay,
            .LightB = solid("Light B", vec4(0.0f)),
            .Temperature = solid("Temperature", vec4(0.0f)),
            .FrameBlend = FlipbookFrameBlend::Crossfade,
        }));
    }

    struct ParticleView
    {
        Unique<SceneRenderer> Renderer;
        CameraView Camera;
        std::vector<u8> Pixels;

        void Render(Context& context, const Scene& scene)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Renderer->Execute(cmd,
                                      Veng::Renderer::SceneView{.World = scene,
                                                                .Camera = Camera,
                                                                .Delta = 0.0f,
                                                                .Exposure = 1.0f,
                                                                .Tonemapper = Tonemapper::None});
                });
            Pixels = Renderer->GetOutput()->GetImage()->Download();
        }

        // The rendered colour at the pixel a world point projects to.
        [[nodiscard]] vec3 At(const vec3 world) const
        {
            const vec4 clip = Camera.ViewProjection() * vec4(world, 1.0f);
            const vec2 ndc = vec2(clip) / clip.w;
            const uvec2 pixel =
                glm::clamp(uvec2((ndc * 0.5f + 0.5f) * vec2(Extent)), uvec2(0u), Extent - 1u);
            const auto* halves = reinterpret_cast<const u16*>(Pixels.data());
            const usize base = (static_cast<usize>(pixel.y) * Extent.x + pixel.x) * 4;
            return {glm::unpackHalf1x16(halves[base]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2])};
        }
    };

    ParticleView MakeRenderer(Context& context, AssetManager& assets, const u32 capacity = Capacity)
    {
        ParticleView render{
            .Renderer = SceneRenderer::Create({
                .Context = context,
                .Assets = assets,
                .OutputFormat = context.GetOutputFormat(),
                .Extent = Extent,
                .Settings =
                    {.Bloom = false, .Shadows = false, .AO = false, .ParticleCapacity = capacity},
            }),
        };
        render.Camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
        render.Camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return render;
    }

    // A billboard emitter of the white flipbook, its particles at rest at the anchor.
    ParticleEmitterDesc Billboard(const AssetHandle<Flipbook>& flipbook, const ParticleBlend blend)
    {
        return ParticleEmitterDesc{
            .Flipbooks = {flipbook},
            .Blend = blend,
            .BurstCount = 1,
            .Lifetime = vec2(10.0f),
            .Size = vec2(0.3f),
        };
    }

    AssetHandle<ParticleEffect> Effect(vector<ParticleEmitterDesc> emitters)
    {
        return AssetManager::Adopt(
            ParticleEffect::Create({.Name = "test", .Emitters = std::move(emitters)}));
    }

    Entity Play(Scene& scene, const AssetHandle<ParticleEffect>& effect, const vec3 position,
                const f32 time)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity, Transform{.Position = position});
        ParticleEffectInstance instance{.Effect = effect, .Seed = 3, .Time = time};
        LatchParticleEffect(scene, entity, instance);
        scene.Add<ParticleEffectInstance>(entity, instance);
        return entity;
    }

    void SetTime(Scene& scene, const Entity entity, const f32 time)
    {
        scene.Get<ParticleEffectInstance>(entity).Time = time;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: a particle draws where its closed-form path puts it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context);

    for (const f32 drag : {0.0f, 1.0f})
    {
        ParticleView render = MakeRenderer(Context, assets);
        const Unique<Scene> scene = Scene::Create(Types);
        ParticleEmitterDesc emitter = Billboard(flipbook, ParticleBlend::Additive);
        emitter.Speed = vec2(3.0f);
        emitter.ConeAxis = vec3(1.0f, 0.0f, 0.0f);
        emitter.ConeAngle = 0.01f;
        emitter.Drag = drag;
        const Entity entity = Play(*scene, Effect({emitter}), vec3(0.0f), 0.0f);

        for (u32 frame = 1; frame <= 5; ++frame)
        {
            SetTime(*scene, entity, 0.1f * static_cast<f32>(frame));
            render.Render(Context, *scene);
        }
        const vec3 expected =
            ParticleOffsetAt(vec3(0.0f), vec3(3.0f, 0.0f, 0.0f), vec3(0.0f), drag, 0.5f);
        CHECK(expected.x > 0.9f);
        CHECK(render.At(expected).x > 0.8f);
        CHECK(render.At(vec3(0.0f)).x < 0.05f);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "particle pass: a fed particle draws where it was placed")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context);
    ParticleEmitterDesc emitter = Billboard(flipbook, ParticleBlend::Additive);
    emitter.BurstCount = 0;
    emitter.Fed = true;
    emitter.IntensityCurve = {{.T = 0.0f, .Value = 0.5f}};
    ParticleView render = MakeRenderer(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = Play(*scene, Effect({emitter}), vec3(0.0f), 0.1f);
    const dvec3 anchor = scene->Get<ParticleEffectInstance>(entity).Anchor;
    auto& feed = scene->Add<ParticleFeed>(entity);
    for (const f32 x : {-1.2f, 1.2f})
    {
        feed.Spawns.push_back({.Position = anchor + dvec3(x, 0.0, 0.0), .Time = 0.05f});
    }
    render.Render(Context, *scene);
    CHECK(render.At(vec3(-1.2f, 0.0f, 0.0f)).x > 0.2f);
    CHECK(render.At(vec3(1.2f, 0.0f, 0.0f)).x > 0.2f);
    CHECK(render.At(vec3(0.0f)).x < 0.05f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: each particle plays the flipbook variant its key picks")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    ParticleView render = MakeRenderer(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);

    // Every particle rests on the anchor and adds a 32nd of its variant's colour there (the output
    // holds no more than 1), so the pixel counts the particles that picked each.
    constexpr u32 Count = 24;
    constexpr f32 Share = 1.0f / 32.0f;
    ParticleEmitterDesc emitter =
        Billboard(SolidFlipbook(Context, 255, 0, 0), ParticleBlend::Additive);
    emitter.Flipbooks.push_back(SolidFlipbook(Context, 0, 255, 0));
    emitter.BurstCount = Count;
    emitter.IntensityCurve = {{.T = 0.0f, .Value = Share}};
    const Entity entity = Play(*scene, Effect({emitter}), vec3(0.0f), 0.0f);
    SetTime(*scene, entity, 0.1f);
    render.Render(Context, *scene);

    u32 green = 0;
    for (u32 index = 0; index < Count; ++index)
    {
        green += ParticleVariantOf(ParticleKey(3, 0, index), 2);
    }
    REQUIRE(green > 0);
    REQUIRE(green < Count);
    const vec3 sum = render.At(vec3(0.0f));
    CHECK(sum.x == doctest::Approx(Share * static_cast<f32>(Count - green)).epsilon(0.02));
    CHECK(sum.y == doctest::Approx(Share * static_cast<f32>(green)).epsilon(0.02));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: a burst spawns exactly its count, and past the pool is clamped")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context);

    for (const u32 burst : {16u, 80u})
    {
        ParticleView render = MakeRenderer(Context, assets);
        const Unique<Scene> scene = Scene::Create(Types);
        ParticleEmitterDesc emitter = Billboard(flipbook, ParticleBlend::Additive);
        emitter.BurstCount = burst;
        const Entity entity = Play(*scene, Effect({emitter}), vec3(0.0f), 0.0f);
        // The GPU's counts are read back a frame late here, so the third frame reports the first.
        for (u32 frame = 0; frame < 3; ++frame)
        {
            SetTime(*scene, entity, 0.05f * static_cast<f32>(frame));
            render.Render(Context, *scene);
        }
        const ParticleStats stats = render.Renderer->GetParticleStats();
        CHECK(stats.Capacity == Capacity);
        CHECK(stats.Live == std::min(burst, Capacity));
        CHECK(stats.Spawned == std::min(burst, Capacity));
        CHECK(stats.Dropped == burst - std::min(burst, Capacity));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: death and an early retire empty the pool and idle the chain")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context);
    ParticleEmitterDesc emitter = Billboard(flipbook, ParticleBlend::Additive);
    emitter.BurstCount = 8;
    emitter.Lifetime = vec2(0.5f);
    const AssetHandle<ParticleEffect> effect = Effect({emitter});

    // Death: past the end time the update kills every particle, and the next frame records none of
    // the chain.
    {
        ParticleView render = MakeRenderer(Context, assets);
        const Unique<Scene> scene = Scene::Create(Types);
        const Entity entity = Play(*scene, effect, vec3(0.0f), 0.1f);
        render.Render(Context, *scene);
        CHECK(render.Renderer->DidRecordPassLastFrame("Particle Update"));
        CHECK(render.Renderer->DidRecordPassLastFrame("Scene Particles"));
        SetTime(*scene, entity, 1.0f);
        render.Render(Context, *scene);
        render.Render(Context, *scene);
        CHECK(render.Renderer->GetParticleStats().Live == 0);
        CHECK_FALSE(render.Renderer->DidRecordPassLastFrame("Particle Update"));
        CHECK_FALSE(render.Renderer->DidRecordPassLastFrame("Scene Particles"));
    }

    // An early retire: the instance leaves mid-life and its particles die at the next update.
    {
        ParticleView render = MakeRenderer(Context, assets);
        const Unique<Scene> scene = Scene::Create(Types);
        const Entity entity = Play(*scene, effect, vec3(0.0f), 0.1f);
        render.Render(Context, *scene);
        render.Render(Context, *scene);
        CHECK(render.Renderer->GetParticleStats().Live == 8);
        scene->DestroyEntity(entity);
        render.Render(Context, *scene);
        render.Render(Context, *scene);
        CHECK(render.Renderer->GetParticleStats().Live == 0);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: alpha particles composite back to front whichever spawned first")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context);
    const auto tinted = [&](const vec3 tint)
    {
        ParticleEmitterDesc emitter = Billboard(flipbook, ParticleBlend::Alpha);
        emitter.Size = vec2(1.0f);
        emitter.TintCurve = {{.T = 0.0f, .Value = tint}};
        return Effect({emitter});
    };
    const AssetHandle<ParticleEffect> red = tinted(vec3(1.0f, 0.0f, 0.0f));
    const AssetHandle<ParticleEffect> green = tinted(vec3(0.0f, 1.0f, 0.0f));

    for (const bool nearFirst : {false, true})
    {
        ParticleView render = MakeRenderer(Context, assets);
        const Unique<Scene> scene = Scene::Create(Types);
        // The camera stands at z = -5 looking down +z, so the green particle is the nearer.
        if (nearFirst)
        {
            (void)Play(*scene, green, vec3(0.0f, 0.0f, -1.0f), 0.1f);
            (void)Play(*scene, red, vec3(0.0f, 0.0f, 1.0f), 0.1f);
        }
        else
        {
            (void)Play(*scene, red, vec3(0.0f, 0.0f, 1.0f), 0.1f);
            (void)Play(*scene, green, vec3(0.0f, 0.0f, -1.0f), 0.1f);
        }
        render.Render(Context, *scene);
        const vec3 centre = render.At(vec3(0.0f));
        CHECK(centre.y > 0.9f);
        CHECK(centre.x < 0.1f);
    }

    // Past one sort block: 1500 farther red particles on a shell, so the sort runs a device-memory
    // stage and a merge, and the one nearer green particle still composites last.
    ParticleEmitterDesc shell = Billboard(flipbook, ParticleBlend::Alpha);
    shell.BurstCount = 1500;
    shell.Size = vec2(2.0f);
    shell.Shape = ParticleShape::Shell;
    shell.ShapeRadius = 0.5f;
    shell.TintCurve = {{.T = 0.0f, .Value = vec3(1.0f, 0.0f, 0.0f)}};
    ParticleView render = MakeRenderer(Context, assets, 2048);
    const Unique<Scene> scene = Scene::Create(Types);
    (void)Play(*scene, green, vec3(0.0f, 0.0f, -1.0f), 0.1f);
    (void)Play(*scene, Effect({shell}), vec3(0.0f, 0.0f, 1.5f), 0.1f);
    // The sort length is read back a frame late here.
    render.Render(Context, *scene);
    render.Render(Context, *scene);
    CHECK(render.Renderer->GetParticleStats().AlphaSorted == 2048);
    const vec3 centre = render.At(vec3(0.0f));
    CHECK(centre.y > 0.9f);
    CHECK(centre.x < 0.1f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: a streak lights the span from its tail to its head")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    ParticleView render = MakeRenderer(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);

    const ParticleEmitterDesc streak{
        .Render = ParticleRender::Streak,
        .Blend = ParticleBlend::Additive,
        .BurstCount = 1,
        .Lifetime = vec2(10.0f),
        .Speed = vec2(4.0f),
        .ConeAxis = vec3(1.0f, 0.0f, 0.0f),
        .ConeAngle = 0.01f,
        .Size = vec2(0.2f),
        .StreakColor = vec3(2.0f),
        .StreakLengthScale = 0.4f,
    };
    const Entity entity = Play(*scene, Effect({streak}), vec3(-1.0f, 0.0f, 0.0f), 0.0f);
    SetTime(*scene, entity, 0.5f);
    render.Render(Context, *scene);

    // The head is at x = 1 and the tail 1.6 behind it; the middle of the span is lit, and the
    // anchor it left long ago is not.
    CHECK(render.At(vec3(0.2f, 0.0f, 0.0f)).x > 0.5f);
    CHECK(render.At(vec3(-1.0f, 0.0f, 0.0f)).x < 0.05f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "particle pass: a six-way particle is lit by the axis the light arrives on")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    ParticleView render = MakeRenderer(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);
    ParticleEmitterDesc emitter = Billboard(SixWayFlipbook(Context), ParticleBlend::Alpha);
    emitter.Size = vec2(2.0f);
    (void)Play(*scene, Effect({emitter}), vec3(0.0f), 0.1f);
    render.Render(Context, *scene);
    const vec3 baseline = render.At(vec3(0.0f));

    // Arriving from the camera's right (+X in the particle's frame), the light reaches lightA's red
    // map at the white-card normalisation, E/pi, gathered once in the update.
    constexpr f32 Irradiance = 2.0f;
    const mat4 view = render.Camera.View();
    const vec3 right = glm::normalize(vec3(view[0][0], view[1][0], view[2][0]));
    const Entity sun = scene->CreateEntity();
    scene->Add<Light>(sun) = Light{.Type = LightType::Directional,
                                   .Direction = -right,
                                   .Color = vec3(1.0f),
                                   .Intensity = Irradiance / LuminousAnchor};
    render.Render(Context, *scene);
    const vec3 lit = render.At(vec3(0.0f));
    CHECK(lit.x - baseline.x == doctest::Approx(Irradiance / glm::pi<f32>()).epsilon(0.02));

    // From the left it reaches only lightB's dark -X map.
    scene->Get<Light>(sun).Direction = right;
    render.Render(Context, *scene);
    CHECK(glm::length(render.At(vec3(0.0f)) - baseline) < 1e-3f);
}
