// Ribbons and trails, device-free: a trail's sample ring stays within MaxSamples and Lifetime and
// empties when its entity stops, a re-base of the origin carries every ribbon and sample with it,
// the frame's packed segments stay bounded by the ribbons plus the trails' samples, and a pooled
// beam fades over its lifetime and returns to the pool.

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/FlipbookSystem.h>
#include <Veng/Scene/RibbonSystem.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/Passes/RibbonScenePass.h"

using namespace Veng;

namespace
{
    constexpr f32 Frame = 1.0f / 60.0f;

    struct RibbonScene
    {
        TypeRegistry Types;
        Unique<Scene> World;
        CameraView Camera;

        RibbonScene()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
            Camera.SetView(vec3(0.0f, 0.0f, 20.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        }

        [[nodiscard]] usize Segments() const
        {
            Renderer::RibbonDrawPlan plan;
            Renderer::GatherRibbons(*World, Camera, 0.0f, plan);
            return plan.GetSegmentCount();
        }

        // Moves the entity to @p position and advances its trail by a frame.
        void Step(const Entity entity, const vec3& position) const
        {
            World->Get<Transform>(entity).Position = position;
            AdvanceTrail(World->Get<Trail>(entity), position, Frame);
        }
    };

    // Per-tick services RibbonSystem and FlipbookSystem never touch.
    SystemContext IdleContext()
    {
        alignas(16) static unsigned char services[64]{};
        return SystemContext{
            .Assets = *reinterpret_cast<AssetManager*>(services),
            .Input = *reinterpret_cast<Input*>(services),
            .Tasks = *reinterpret_cast<TaskSystem*>(services),
            .Audio = *reinterpret_cast<Audio::AudioEngine*>(services),
            .Localization = *reinterpret_cast<Localization::Localization*>(services),
        };
    }
}

TEST_CASE("A trail holds at most MaxSamples, and none older than its lifetime")
{
    // A short ring the lifetime would overflow, and a long one the lifetime bounds instead.
    for (const Trail shape :
         {Trail{.Lifetime = 0.5f, .MaxSamples = 8}, Trail{.Lifetime = 0.25f, .MaxSamples = 64}})
    {
        Trail trail = shape;
        usize most = 0;
        f32 oldest = 0.0f;
        for (u32 frame = 0; frame < 120; ++frame)
        {
            AdvanceTrail(trail, vec3(static_cast<f32>(frame), 0.0f, 0.0f), Frame);
            most = std::max(most, trail.Samples.size());
            for (const TrailSample& sample : trail.Samples)
            {
                oldest = std::max(oldest, sample.Age);
            }
        }
        CHECK(most <= shape.MaxSamples);
        CHECK(oldest < shape.Lifetime);
        CHECK_FALSE(trail.Samples.empty());
    }
}

TEST_CASE(
    "A trail draws segments while its entity moves, and none once it has stood still a lifetime")
{
    RibbonScene fixture;
    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover);
    AttachTrail(*fixture.World, mover, Trail{.Lifetime = 0.5f, .MaxSamples = 32});

    for (u32 frame = 0; frame < 20; ++frame)
    {
        fixture.Step(mover, vec3(0.1f * static_cast<f32>(frame), 0.0f, 0.0f));
    }
    CHECK(fixture.Segments() > 0);

    const vec3 rest = fixture.World->Get<Transform>(mover).Position;
    for (u32 frame = 0; frame < 32; ++frame)
    {
        fixture.Step(mover, rest);
    }
    CHECK(fixture.Segments() == 0);

    // Re-attaching starts the trail fresh wherever the entity now stands.
    fixture.Step(mover, rest + vec3(5.0f, 0.0f, 0.0f));
    AttachTrail(*fixture.World, mover, Trail{.Lifetime = 0.5f});
    CHECK(fixture.World->Get<Trail>(mover).Samples.empty());
}

TEST_CASE("A re-based origin carries every ribbon and trail sample with it, and their shape too")
{
    RibbonScene scene;
    const Entity streak = scene.World->CreateEntity();
    scene.World->Add<Transform>(streak);
    scene.World->Add<Trail>(streak, Trail{.Lifetime = 1.0f, .MaxSamples = 16});
    for (u32 frame = 0; frame < 8; ++frame)
    {
        scene.Step(streak, vec3(static_cast<f32>(frame), 0.5f * static_cast<f32>(frame), 0.0f));
    }
    const Entity beam = scene.World->CreateEntity();
    scene.World->Add<Ribbon>(beam, Ribbon{.From = vec3(1.0f, 2.0f, 3.0f), .To = vec3(-4.0f)});

    const vector<TrailSample> before = scene.World->Get<Trail>(streak).Samples;
    const Ribbon ribbonBefore = scene.World->Get<Ribbon>(beam);
    const vec3 offset(-120.0f, 3.5f, 42.0f);
    OffsetRibbons(*scene.World, offset);

    const vector<TrailSample>& after = scene.World->Get<Trail>(streak).Samples;
    REQUIRE(after.size() == before.size());
    f32 worst = 0.0f;
    for (usize i = 0; i < after.size(); ++i)
    {
        worst = std::max(worst, glm::length(after[i].Position - (before[i].Position + offset)));
        CHECK(after[i].Age == before[i].Age);
    }
    CHECK(worst < 1e-4f);
    const Ribbon& ribbonAfter = scene.World->Get<Ribbon>(beam);
    CHECK(glm::length(ribbonAfter.From - (ribbonBefore.From + offset)) < 1e-5f);
    CHECK(glm::length(ribbonAfter.To - (ribbonBefore.To + offset)) < 1e-5f);
}

TEST_CASE("A frame's packed segments are bounded by its ribbons plus its trails' samples")
{
    RibbonScene fixture;
    Scene& scene = *fixture.World;

    constexpr u32 ribbons = 5;
    for (u32 i = 0; i < ribbons; ++i)
    {
        const Entity entity = scene.CreateEntity();
        const auto y = static_cast<f32>(i);
        scene.Add<Ribbon>(entity, Ribbon{.From = vec3(-3.0f, y, 0.0f),
                                         .To = vec3(3.0f, y, 0.0f),
                                         .Additive = i % 2 == 0});
    }

    // Trails of several capacities, one of them no longer emitting.
    const vector<u32> capacities{4, 16, 40};
    vector<Entity> trails;
    u32 samples = 0;
    for (const u32 capacity : capacities)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        AttachTrail(scene, entity,
                    Trail{.Lifetime = 1.0f, .MaxSamples = capacity, .Additive = capacity != 16});
        trails.push_back(entity);
        samples += capacity;
    }
    for (u32 frame = 0; frame < 60; ++frame)
    {
        for (usize i = 0; i < trails.size(); ++i)
        {
            fixture.Step(trails[i],
                         vec3(0.2f * static_cast<f32>(frame), static_cast<f32>(i), 1.0f));
        }
    }
    scene.Get<Trail>(trails[0]).Emitting = false;

    Renderer::RibbonDrawPlan plan;
    Renderer::GatherRibbons(scene, fixture.Camera, 0.0f, plan);
    CHECK(plan.GetSegmentCount() >= ribbons);
    CHECK(plan.GetSegmentCount() <= ribbons + samples);
    CHECK(plan.Dropped == 0);
    CHECK_FALSE(plan.Alpha.empty());
    CHECK_FALSE(plan.Additive.empty());

    // Every uploaded position is relative to the eye, so it sits within the scene's own reach of it.
    f32 farthest = 0.0f;
    for (const auto* set : {&plan.Alpha, &plan.Additive})
    {
        for (const Renderer::GpuRibbonSegment& segment : *set)
        {
            farthest = std::max(
                {farthest, glm::length(vec3(segment.Start)), glm::length(vec3(segment.End))});
        }
    }
    CHECK(farthest < 40.0f);
}

TEST_CASE("A transient beam fades over its lifetime, then returns to the pool")
{
    RibbonScene fixture;
    Scene& scene = *fixture.World;
    const SystemContext context = IdleContext();
    RibbonSystem ribbons;
    FlipbookSystem flipbooks;

    const Entity beam = SpawnTransientBeam(
        scene, Ribbon{.From = vec3(0.0f), .To = vec3(0.0f, 0.0f, -10.0f), .ColorFrom = vec3(4.0f)},
        0.1f);
    REQUIRE(scene.GetEffectPool() != nullptr);
    REQUIRE(scene.Has<Ribbon>(beam));
    CHECK(scene.Get<Ribbon>(beam).Lifetime == doctest::Approx(0.1f));
    CHECK_FALSE(scene.Has<FlipbookSprite>(beam));

    const auto opacity = [&]()
    {
        Renderer::RibbonDrawPlan plan;
        Renderer::GatherRibbons(scene, fixture.Camera, 0.0f, plan);
        return plan.Additive.empty() ? 0.0f : plan.Additive.front().StartColor.a;
    };

    // The fade only ever falls while the beam lives.
    f32 previous = opacity();
    CHECK(previous == doctest::Approx(1.0f));
    bool rose = false;
    for (u32 frame = 0; frame < 5; ++frame)
    {
        ribbons.OnUpdate(scene, 0.015f, context);
        flipbooks.OnUpdate(scene, 0.015f, context);
        const f32 now = opacity();
        rose = rose || now > previous;
        previous = now;
    }
    CHECK_FALSE(rose);
    CHECK(previous < 1.0f);
    CHECK(scene.GetEffectPool()->IsLive(beam));

    for (u32 frame = 0; frame < 5; ++frame)
    {
        ribbons.OnUpdate(scene, 0.015f, context);
        flipbooks.OnUpdate(scene, 0.015f, context);
    }
    CHECK_FALSE(scene.GetEffectPool()->IsLive(beam));
    CHECK_FALSE(scene.Has<Ribbon>(beam));
    CHECK(opacity() == 0.0f);
}
