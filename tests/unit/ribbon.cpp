// Ribbons, trails and ribbon paths, device-free: a trail's sample ring stays within MaxSamples and
// Lifetime and empties when its entity stops, a re-base of the origin carries every ribbon and
// sample with it, the frame's packed segments stay bounded by the ribbons plus the trails' samples,
// a pooled beam fades over its lifetime and returns to the pool, a path's strips join into the
// expected segments with shared joints, placed by their entity's drawn pose, a one-point strip is a
// dot of its width, and a path's placement routes it to the scene or the post-resolve plan under
// one shared budget. A trail's cross-section draws the extent of its emitter's shape across the side
// it is seen from; a sample fired from its emitter leaves along its axis, slows to rest by a path
// the frame rate does not change, inherits the emitter's motion, and sweeps up the slower samples it
// overtakes rather than doubling the trail back; and a tube trail gathers a
// segment per sample pair around its outline, tapering with its age.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/EffectPool.h>
#include <Veng/Scene/FlipbookSystem.h>
#include <Veng/Scene/RibbonSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include "support/TestServices.h"

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

        [[nodiscard]] usize Segments() const { return Gather().GetSegmentCount(); }

        // Gathers the scene's records at @p alpha into the scene plan and @p postResolve.
        [[nodiscard]] Renderer::RibbonDrawPlan Gather(const f32 alpha,
                                                      Renderer::RibbonDrawPlan& postResolve) const
        {
            Renderer::RibbonDrawPlan plan;
            Renderer::GatherRibbons(*World, Camera, alpha, plan, postResolve);
            return plan;
        }

        // Gathers the scene's scene-placed records at @p alpha.
        [[nodiscard]] Renderer::RibbonDrawPlan Gather(const f32 alpha = 0.0f) const
        {
            Renderer::RibbonDrawPlan postResolve;
            return Gather(alpha, postResolve);
        }

        // Gathers the scene's post-resolve-placed records.
        [[nodiscard]] Renderer::RibbonDrawPlan GatherPostResolve() const
        {
            Renderer::RibbonDrawPlan postResolve;
            (void)Gather(0.0f, postResolve);
            return postResolve;
        }

        // Stands a path of the given strips on a new entity at @p pose.
        Entity AddPath(const vector<RibbonStrip>& strips, const Transform& pose = {},
                       const bool additive = true,
                       const RibbonPlacement placement = RibbonPlacement::Scene) const
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, pose);
            World->Add<RibbonPath>(
                entity, RibbonPath{.Strips = strips, .Additive = additive, .Placement = placement});
            return entity;
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
        return TestSupport::SharedTestServices().Make();
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

TEST_CASE("A trail's cross-section shows its extent across the side it is seen from")
{
    // A 3 x 1 ellipse across a trail running along -z.
    const vec3 wide(1.5f, 0.0f, 0.0f);
    const vec3 thin(0.0f, 0.5f, 0.0f);
    const vec3 along(0.0f, 0.0f, -1.0f);

    // Seen from above, the band runs across x: the wide extent. Seen from the side, the thin one.
    CHECK(TrailCrossSectionWidth(wide, thin, along, vec3(0.0f, -10.0f, 0.0f)) ==
          doctest::Approx(3.0f));
    CHECK(TrailCrossSectionWidth(wide, thin, along, vec3(-10.0f, 0.0f, 0.0f)) ==
          doctest::Approx(1.0f));

    // From any side it lies between the two, and a circle shows its diameter from every side.
    f32 least = 1e9f;
    f32 most = 0.0f;
    f32 circleLeast = 1e9f;
    f32 circleMost = 0.0f;
    for (int step = 0; step < 24; ++step)
    {
        const f32 angle = static_cast<f32>(step) * std::numbers::pi_v<f32> / 12.0f;
        const vec3 eyeToPoint(10.0f * std::cos(angle), 10.0f * std::sin(angle), 3.0f);
        const f32 width = TrailCrossSectionWidth(wide, thin, along, eyeToPoint);
        least = std::min(least, width);
        most = std::max(most, width);
        const f32 circle = TrailCrossSectionWidth(vec3(0.7f, 0.0f, 0.0f), vec3(0.0f, 0.7f, 0.0f),
                                                  along, eyeToPoint);
        circleLeast = std::min(circleLeast, circle);
        circleMost = std::max(circleMost, circle);
    }
    CHECK(least >= 1.0f - 1e-4f);
    CHECK(most <= 3.0f + 1e-4f);
    CHECK(circleLeast == doctest::Approx(1.4f));
    CHECK(circleMost == doctest::Approx(1.4f));

    // Looking straight down the trail still draws a band within the ellipse's extents.
    const f32 endOn = TrailCrossSectionWidth(wide, thin, along, vec3(0.0f, 0.0f, -10.0f));
    CHECK(endOn >= 1.0f - 1e-4f);
    CHECK(endOn <= 3.0f + 1e-4f);
}

TEST_CASE("A trail records its emitter's axes and draws its cross-section from the viewer's side")
{
    RibbonScene fixture;
    Scene& scene = *fixture.World;

    // Turned a quarter about y and doubled, the emitter's local x lies along world -z and its local
    // z along world x, the way it flies; a 1.5 x 0.5 cross-section is 3 m deep and 1 m tall in world.
    const quat turn = glm::angleAxis(std::numbers::pi_v<f32> / 2.0f, vec3(0.0f, 1.0f, 0.0f));
    const Entity emitter = scene.CreateEntity();
    scene.Add<Transform>(emitter, Transform{.Rotation = turn, .Scale = vec3(2.0f)});
    AttachTrail(
        scene, emitter,
        Trail{
            .Lifetime = 10.0f, .Width = 1.0f, .CrossSection = vec2(1.5f, 0.5f), .MaxSamples = 64});
    for (u32 frame = 0; frame <= 30; ++frame)
    {
        scene.Get<Transform>(emitter).Position =
            vec3(-3.0f + (0.2f * static_cast<f32>(frame)), 0.0f, 0.0f);
        AdvanceTrail(scene.Get<Trail>(emitter), WorldMatrix(scene, emitter), Frame);
    }
    const TrailSample& sample = scene.Get<Trail>(emitter).Samples.back();
    CHECK(glm::distance(sample.AxisX, vec3(0.0f, 0.0f, -2.0f)) < 1e-4f);
    CHECK(glm::distance(sample.AxisY, vec3(0.0f, 2.0f, 0.0f)) < 1e-4f);

    // The extremes of every drawn end's width, from wherever the camera stands.
    const auto widths = [&fixture]
    {
        f32 least = 1e9f;
        f32 most = 0.0f;
        for (const Renderer::GpuRibbonSegment& segment : fixture.Gather().Additive)
        {
            least = std::min({least, segment.Start.w, segment.End.w});
            most = std::max({most, segment.Start.w, segment.End.w});
        }
        return std::pair{least, most};
    };

    // From the side (the fixture's camera, out along +z) the band is the 1 m height.
    const auto [sideLeast, sideMost] = widths();
    CHECK(sideLeast == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(sideMost == doctest::Approx(1.0f).epsilon(1e-3));

    // From overhead it is the 3 m depth.
    fixture.Camera.SetView(vec3(0.0f, 20.0f, 0.0f), vec3(0.0f), vec3(0.0f, 0.0f, -1.0f));
    const auto [topLeast, topMost] = widths();
    CHECK(topLeast == doctest::Approx(3.0f).epsilon(1e-3));
    CHECK(topMost == doctest::Approx(3.0f).epsilon(1e-3));
}

TEST_CASE("A sample leaves along its emitter's axis, never speeds up, and comes to rest")
{
    // Turned a quarter about y, the emitter's local -z points along world -x.
    const quat turn = glm::angleAxis(std::numbers::pi_v<f32> / 2.0f, vec3(0.0f, 1.0f, 0.0f));
    const mat4 head = glm::translate(mat4(1.0f), vec3(1.0f, 2.0f, 3.0f)) * glm::mat4_cast(turn);

    // The travel of a trail's first sample over a second, at a given frame rate.
    const auto flown = [&head](const u32 frames)
    {
        Trail trail{.Lifetime = 10.0f,
                    .EmitVelocity = vec3(0.0f, 0.0f, -10.0f),
                    .Drag = 2.0f,
                    .MinSampleDistance = 1e9f};
        AdvanceTrail(trail, head, 1.0f / static_cast<f32>(frames));
        const vec3 start = trail.Samples.front().Position;
        f32 previous = glm::length(trail.Samples.front().Velocity);
        bool slowing = true;
        bool straight = true;
        for (u32 frame = 0; frame < frames; ++frame)
        {
            AdvanceTrail(trail, head, 1.0f / static_cast<f32>(frames));
            const TrailSample& sample = trail.Samples.front();
            const f32 speed = glm::length(sample.Velocity);
            slowing = slowing && speed <= previous;
            previous = speed;
            const vec3 travel = sample.Position - start;
            straight = straight && std::abs(travel.y) < 1e-4f && std::abs(travel.z) < 1e-4f &&
                       travel.x <= 0.0f;
        }
        CHECK(slowing);
        CHECK(straight);
        return glm::length(trail.Samples.front().Position - start);
    };

    // It leaves at the emit speed along the emitter's axis.
    Trail fresh{.EmitVelocity = vec3(0.0f, 0.0f, -10.0f)};
    AdvanceTrail(fresh, head, 0.0f);
    CHECK(glm::distance(fresh.Samples.front().Velocity, vec3(-10.0f, 0.0f, 0.0f)) < 1e-4f);

    // Its path over a second does not depend on the frame rate, and is within the rest distance.
    const f32 at60 = flown(60);
    const f32 at15 = flown(15);
    CHECK(at60 == doctest::Approx(at15).epsilon(1e-4));
    CHECK(at60 < 10.0f / 2.0f);
    CHECK(at60 > 0.8f * (10.0f / 2.0f));
}

TEST_CASE("A sample inherits its emitter's velocity, and a restart forgets where the head stood")
{
    // A whole inheritance and no drag: every sample keeps pace with an emitter flying straight.
    Trail trail{.Lifetime = 10.0f, .InheritVelocity = 1.0f};
    f32 lagging = 0.0f;
    for (u32 frame = 0; frame <= 30; ++frame)
    {
        const vec3 position(static_cast<f32>(frame), 0.0f, 0.0f);
        AdvanceTrail(trail, position, Frame);
        for (const TrailSample& sample : trail.Samples)
        {
            // The first sample of a run has no velocity to inherit yet.
            if (sample.Velocity != vec3(0.0f))
            {
                lagging = std::max(lagging, glm::distance(sample.Position, position));
            }
        }
    }
    CHECK(lagging < 1e-3f);

    // After a restart, the jump to wherever the emitter now stands lends its first sample nothing.
    RestartTrail(trail);
    CHECK(trail.Samples.empty());
    AdvanceTrail(trail, vec3(1000.0f, 0.0f, 0.0f), Frame);
    CHECK(trail.Samples.front().Velocity == vec3(0.0f));
}

TEST_CASE("A supplied emitter velocity holds a stepped emitter's samples to one, for one advance")
{
    // The head moves two units every other frame: one unit a frame on average, read as 0 and 2.
    // Fired back at twice the emitter's speed, so the samples stream out behind it.
    const vec3 velocity(1.0f / Frame, 0.0f, 0.0f);
    const vec3 exhaust(-2.0f / Frame, 0.0f, 0.0f);
    Trail trail{.Lifetime = 10.0f, .EmitVelocity = exhaust, .InheritVelocity = 1.0f};
    f32 spread = 0.0f;
    for (u32 frame = 0; frame <= 30; ++frame)
    {
        trail.EmitterVelocity = velocity;
        trail.HasEmitterVelocity = true;
        AdvanceTrail(trail, vec3(static_cast<f32>(frame & ~1U), 0.0f, 0.0f), Frame);
    }
    for (const TrailSample& sample : trail.Samples)
    {
        spread = std::max(spread, glm::distance(sample.Velocity, velocity + exhaust));
    }
    CHECK(trail.Samples.size() > 10);
    CHECK(spread < 1e-3f);
    CHECK_FALSE(trail.HasEmitterVelocity);

    // Unsupplied, the next advance reads the head's own travel again.
    AdvanceTrail(trail, vec3(40.0f, 0.0f, 0.0f), Frame);
    CHECK(trail.Samples.back().Velocity.x == doctest::Approx(8.0f / Frame));
}

TEST_CASE("A faster sample sweeps up the slower ones it overtakes, so the trail never doubles back")
{
    // The exhaust speeds up and the emitter accelerates after it: each new sample is faster than
    // those ahead of it, and comes to rest farther out, so unswept it would pass them.
    Trail trail{
        .Lifetime = 0.9f, .InheritVelocity = 1.0f, .Drag = 4.0f, .MinSampleDistance = 10.0f};
    f32 position = 0.0f;
    usize doubledBack = 0;
    usize longest = 0;
    for (u32 frame = 0; frame < 180; ++frame)
    {
        const f32 ramp = std::clamp(static_cast<f32>(frame) / 60.0f, 0.0f, 1.0f);
        trail.EmitVelocity = vec3(0.0f, 0.0f, 60.0f + (340.0f * ramp));
        position -= 200.0f * ramp * Frame;
        const vec3 head(0.0f, 0.0f, position);
        AdvanceTrail(trail, head, Frame);

        vector<vec3> chain;
        for (const TrailSample& sample : trail.Samples)
        {
            chain.push_back(sample.Position);
        }
        chain.push_back(head);
        for (usize i = 2; i < chain.size(); ++i)
        {
            doubledBack += glm::dot(chain[i - 1] - chain[i - 2], chain[i] - chain[i - 1]) < 0.0f;
        }
        longest = std::max(longest, trail.Samples.size());
    }
    CHECK(doubledBack == 0);
    CHECK(longest > 4);

    // A trail whose samples stay put keeps every one, even where its emitter turns straight back.
    Trail still{.Lifetime = 10.0f};
    for (const f32 x : {0.0f, 1.0f, 2.0f, 1.0f, 0.0f})
    {
        AdvanceTrail(still, vec3(x, 0.0f, 0.0f), Frame);
    }
    CHECK(still.Samples.size() == 5);
}

TEST_CASE("A tube trail gathers a segment per sample pair around its outline, and a band none")
{
    RibbonScene fixture;
    Scene& scene = *fixture.World;
    const vector<vec2> square{vec2(-1.0f, -1.0f), vec2(1.0f, -1.0f), vec2(1.0f, 1.0f),
                              vec2(-1.0f, 1.0f)};

    const auto stand = [&](const TrailShape shape)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        AttachTrail(scene, entity,
                    Trail{.Lifetime = 1.0f,
                          .Width = 2.0f,
                          .Shape = shape,
                          .Outline = square,
                          .TailWidthScale = 0.5f,
                          .MaxSamples = 64});
        for (u32 frame = 0; frame < 10; ++frame)
        {
            fixture.Step(entity, vec3(static_cast<f32>(frame), 0.0f, 0.0f));
        }
        scene.Get<Trail>(entity).Emitting = false;
        return entity;
    };

    const Entity band = stand(TrailShape::Band);
    Renderer::RibbonDrawPlan plan = fixture.Gather();
    CHECK(plan.AdditiveTubes.empty());
    CHECK(plan.Outlines.empty());
    scene.DestroyEntity(band);

    const Entity tube = stand(TrailShape::Tube);
    plan = fixture.Gather();
    const usize samples = scene.Get<Trail>(tube).Samples.size();
    CHECK(plan.Additive.empty());
    CHECK(plan.AdditiveTubes.size() == samples - 1);
    REQUIRE(plan.Outlines.size() == square.size());

    // Each ring's axes carry the taper: the newest end at the full width, older ends narrower, and
    // never wider than full.
    f32 widest = 0.0f;
    f32 narrowest = 1e9f;
    for (const Renderer::GpuTrailTube& record : plan.AdditiveTubes)
    {
        CHECK(record.Outline == uvec4(0u, 4u, 0u, 0u));
        for (const vec4& axis : {record.StartAxisX, record.EndAxisX})
        {
            const f32 width = glm::length(vec3(axis));
            widest = std::max(widest, width);
            narrowest = std::min(narrowest, width);
        }
    }
    CHECK(widest <= 2.0f + 1e-4f);
    CHECK(widest > 1.9f);
    CHECK(narrowest < widest);
    CHECK(narrowest >= 1.0f - 1e-4f);
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

    const Renderer::RibbonDrawPlan plan = fixture.Gather();
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
        const Renderer::RibbonDrawPlan plan = fixture.Gather();
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

namespace
{
    // The world position a segment end stands at, undoing the gather's rebase to the eye.
    vec3 WorldPoint(const RibbonScene& scene, const vec4& relative)
    {
        return vec3(relative) + scene.Camera.GetPosition();
    }

    // N points evenly round a circle of @p radius in the local XY plane.
    vector<vec3> Circle(const u32 count, const f32 radius)
    {
        vector<vec3> points;
        for (u32 i = 0; i < count; ++i)
        {
            const f32 angle =
                2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / static_cast<f32>(count);
            points.emplace_back(radius * std::cos(angle), radius * std::sin(angle), 0.0f);
        }
        return points;
    }
}

TEST_CASE("A closed strip of N points draws N segments, every joint shared, the seam included")
{
    const RibbonScene fixture;
    constexpr u32 count = 24;
    fixture.AddPath({RibbonStrip{.Points = Circle(count, 2.0f), .Closed = true}});

    const Renderer::RibbonDrawPlan plan = fixture.Gather();
    REQUIRE(plan.Additive.size() == count);
    CHECK(plan.Alpha.empty());

    // Each segment's end tangent is its successor's start tangent, wrapping from the last to the
    // first; and on a regular polygon a joint's tangent is square to its radius.
    f32 worstShared = 0.0f;
    f32 worstSquare = 0.0f;
    for (usize i = 0; i < count; ++i)
    {
        const Renderer::GpuRibbonSegment& segment = plan.Additive[i];
        const Renderer::GpuRibbonSegment& next = plan.Additive[(i + 1) % count];
        worstShared =
            std::max(worstShared, glm::length(vec3(segment.EndTangent) - vec3(next.StartTangent)));
        worstShared = std::max(worstShared, glm::length(WorldPoint(fixture, segment.End) -
                                                        WorldPoint(fixture, next.Start)));
        worstSquare = std::max(worstSquare, std::abs(glm::dot(vec3(segment.StartTangent),
                                                              WorldPoint(fixture, segment.Start))));
    }
    CHECK(worstShared < 1e-4f);
    CHECK(worstSquare < 1e-4f);

    // The first point repeated at the end closes the strip once, not twice.
    vector<vec3> repeated = Circle(count, 2.0f);
    repeated.push_back(repeated.front());
    const RibbonScene again;
    again.AddPath({RibbonStrip{.Points = repeated, .Closed = true}});
    CHECK(again.Gather().GetSegmentCount() == count);
}

TEST_CASE("An open strip of N distinct points draws N - 1 segments, merging coincident points")
{
    const RibbonScene fixture;
    const vector<vec3> zigzag{vec3(0.0f), vec3(1.0f, 1.0f, 0.0f), vec3(2.0f, 0.0f, 0.0f),
                              vec3(3.0f, 1.0f, 0.0f), vec3(4.0f, 0.0f, 0.0f)};
    // The same five points, each doubled or nudged within the coincidence distance.
    vector<vec3> stuttered;
    for (const vec3& point : zigzag)
    {
        stuttered.push_back(point);
        stuttered.push_back(point + vec3(0.0f, 0.0f, 1e-6f));
    }
    fixture.AddPath({RibbonStrip{.Points = zigzag}, RibbonStrip{.Points = stuttered}}, Transform{},
                    false);

    const Renderer::RibbonDrawPlan plan = fixture.Gather();
    CHECK(plan.Additive.empty());
    CHECK(plan.Alpha.size() == 2 * (zigzag.size() - 1));

    // A hairpin's joint still faces somewhere: no tangent degenerates.
    const RibbonScene hairpin;
    hairpin.AddPath({RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f, 0.0f, 0.0f), vec3(0.0f)}}});
    const Renderer::RibbonDrawPlan back = hairpin.Gather();
    REQUIRE(back.Additive.size() == 2);
    f32 shortest = 1.0f;
    for (const Renderer::GpuRibbonSegment& segment : back.Additive)
    {
        shortest = std::min({shortest, glm::length(vec3(segment.StartTangent)),
                             glm::length(vec3(segment.EndTangent))});
    }
    CHECK(shortest == doctest::Approx(1.0f));
}

TEST_CASE("A strip of no points, or none visible, draws nothing")
{
    RibbonScene fixture;
    fixture.AddPath({
        RibbonStrip{},
        RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f)}, .Opacity = 0.0f},
        RibbonStrip{.Points = {vec3(1.0f)}, .Opacity = 0.0f},
    });
    // A path off a Transform-less entity draws nothing either.
    const Entity loose = fixture.World->CreateEntity();
    fixture.World->Add<RibbonPath>(
        loose, RibbonPath{.Strips = {RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f)}}}});
    CHECK(fixture.Gather().IsEmpty());

    // Two distinct points closed draw their one segment, not a doubled-back pair.
    fixture.AddPath({RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f)}, .Closed = true}});
    CHECK(fixture.Gather().GetSegmentCount() == 1);
}

TEST_CASE("A path's points and widths follow its entity's translation, rotation and scale")
{
    const RibbonScene fixture;
    const Transform pose{
        .Position = vec3(3.0f, -2.0f, 1.0f),
        .Rotation = glm::angleAxis(0.7f, glm::normalize(vec3(1.0f, 2.0f, 0.5f))),
        .Scale = vec3(2.5f),
    };
    const vector<vec3> local{vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, -1.0f),
                             vec3(-1.0f, 0.5f, 2.0f)};
    fixture.AddPath({RibbonStrip{.Points = local, .Width = 0.04f}}, pose);

    const Renderer::RibbonDrawPlan plan = fixture.Gather();
    REQUIRE(plan.Additive.size() == 2);
    const auto expected = [&pose](const vec3& point)
    { return pose.Position + pose.Rotation * (pose.Scale * point); };
    f32 worst = 0.0f;
    f32 widest = 0.0f;
    f32 narrowest = 1e9f;
    for (usize i = 0; i < plan.Additive.size(); ++i)
    {
        const Renderer::GpuRibbonSegment& segment = plan.Additive[i];
        worst =
            std::max({worst, glm::length(WorldPoint(fixture, segment.Start) - expected(local[i])),
                      glm::length(WorldPoint(fixture, segment.End) - expected(local[i + 1]))});
        widest = std::max({widest, segment.Start.w, segment.End.w});
        narrowest = std::min({narrowest, segment.Start.w, segment.End.w});
    }
    CHECK(worst < 1e-4f);
    CHECK(narrowest == doctest::Approx(0.04f * 2.5f));
    CHECK(widest == doctest::Approx(0.04f * 2.5f));
}

TEST_CASE("A path rides its entity's interpolated pose between two ticks")
{
    RibbonScene fixture;
    const Entity entity =
        fixture.AddPath({RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f, 0.0f, 0.0f)}}});
    fixture.World->SnapshotTransformHistory();
    fixture.World->Get<Transform>(entity).Position = vec3(0.0f, 4.0f, 0.0f);
    fixture.World->SnapshotTransformHistory();
    REQUIRE(fixture.World->HasTransformInterpolation());

    // Half a tick on, the path stands halfway between the two poses, as the entity's meshes do.
    const Renderer::RibbonDrawPlan plan = fixture.Gather(0.5f);
    REQUIRE(plan.Additive.size() == 1);
    CHECK(glm::length(WorldPoint(fixture, plan.Additive.front().Start) - vec3(0.0f, 2.0f, 0.0f)) <
          1e-4f);
}

TEST_CASE("A one-point strip is one dot of its width at its transformed point, in either placement")
{
    const Transform pose{
        .Position = vec3(-2.0f, 1.0f, 3.0f),
        .Rotation = glm::angleAxis(1.1f, glm::normalize(vec3(0.3f, 1.0f, -0.2f))),
        .Scale = vec3(1.5f),
    };
    const vec3 local(0.5f, -1.0f, 2.0f);
    const vec3 expected = pose.Position + pose.Rotation * (pose.Scale * local);
    const RibbonStrip strip{
        .Points = {local}, .Width = 0.2f, .Color = vec3(3.0f, 2.0f, 1.0f), .Opacity = 0.5f};

    for (const RibbonPlacement placement : {RibbonPlacement::Scene, RibbonPlacement::PostResolve})
    {
        const RibbonScene fixture;
        fixture.AddPath({strip}, pose, /*additive=*/false, placement);
        Renderer::RibbonDrawPlan postResolve;
        const Renderer::RibbonDrawPlan scene = fixture.Gather(0.0f, postResolve);
        const Renderer::RibbonDrawPlan& drawn =
            placement == RibbonPlacement::Scene ? scene : postResolve;
        const Renderer::RibbonDrawPlan& other =
            placement == RibbonPlacement::Scene ? postResolve : scene;

        REQUIRE(drawn.Alpha.size() == 1);
        CHECK(drawn.Additive.empty());
        CHECK(other.IsEmpty());
        const Renderer::GpuRibbonSegment& dot = drawn.Alpha.front();
        CHECK(Renderer::IsDot(dot));
        CHECK(glm::length(WorldPoint(fixture, dot.Start) - expected) < 1e-4f);
        CHECK(glm::length(WorldPoint(fixture, dot.End) - expected) < 1e-4f);
        CHECK(dot.Start.w == doctest::Approx(0.2f * 1.5f));
        CHECK(dot.StartColor == vec4(3.0f, 2.0f, 1.0f, 0.5f));
    }
}

TEST_CASE("Coincident points collapse to one dot, open or closed, and a band is never a dot")
{
    const RibbonScene fixture;
    const vec3 at(1.0f, 2.0f, -1.0f);
    fixture.AddPath({
        RibbonStrip{.Points = {at, at, at + vec3(0.0f, 0.0f, 1e-5f)}},
        RibbonStrip{.Points = {at, at + vec3(1e-6f), at}, .Closed = true},
        RibbonStrip{.Points = {vec3(0.0f), vec3(1.0f, 0.0f, 0.0f), vec3(1.0f)}},
    });

    const Renderer::RibbonDrawPlan plan = fixture.Gather();
    REQUIRE(plan.Additive.size() == 4);
    usize dots = 0;
    for (const Renderer::GpuRibbonSegment& record : plan.Additive)
    {
        if (Renderer::IsDot(record))
        {
            ++dots;
            CHECK(glm::length(WorldPoint(fixture, record.Start) - at) < 1e-4f);
        }
    }
    CHECK(dots == 2);
}

TEST_CASE("A path's placement routes it to the scene or the post-resolve plan; others stay scene")
{
    RibbonScene fixture;
    const vector<vec3> line{vec3(0.0f), vec3(1.0f, 0.0f, 0.0f), vec3(2.0f, 1.0f, 0.0f)};
    fixture.AddPath({RibbonStrip{.Points = line}}, Transform{}, true, RibbonPlacement::Scene);
    fixture.AddPath({RibbonStrip{.Points = line}, RibbonStrip{.Points = {vec3(4.0f)}}}, Transform{},
                    false, RibbonPlacement::PostResolve);
    fixture.AddPath({RibbonStrip{.Points = line}}, Transform{}, true, RibbonPlacement::PostResolve);
    const Entity beam = fixture.World->CreateEntity();
    fixture.World->Add<Ribbon>(beam, Ribbon{.From = vec3(-1.0f), .To = vec3(1.0f)});

    Renderer::RibbonDrawPlan postResolve;
    const Renderer::RibbonDrawPlan scene = fixture.Gather(0.0f, postResolve);
    // The scene-placed path's two segments and the beam stay in the scene plan.
    CHECK(scene.Additive.size() == 3);
    CHECK(scene.Alpha.empty());
    // The post-resolve paths keep their own compositing: two alpha segments and the dot, sorted,
    // and two additive segments.
    CHECK(postResolve.Alpha.size() == 3);
    CHECK(postResolve.Additive.size() == 2);
    CHECK(scene.Dropped == 0);
    CHECK(postResolve.Dropped == 0);
}

TEST_CASE("A view gathers no ribbon on a layer its mask omits, nor its excluded entity")
{
    RibbonScene fixture;
    Scene& world = *fixture.World;
    const Entity shown = world.CreateEntity();
    world.Add<Ribbon>(shown, Ribbon{.From = vec3(-1.0f), .To = vec3(1.0f)});
    const Entity display = world.CreateEntity();
    world.Add<Ribbon>(display,
                      Ribbon{.From = vec3(-1.0f), .To = vec3(1.0f), .Layer = RenderLayer::Display});
    const Entity excluded = world.CreateEntity();
    world.Add<Ribbon>(excluded, Ribbon{.From = vec3(-1.0f), .To = vec3(1.0f)});

    const auto gathered = [&](const u32 layers, const Entity exclude)
    {
        Renderer::RibbonDrawPlan plan;
        Renderer::RibbonDrawPlan postResolve;
        Renderer::GatherRibbons(world, fixture.Camera, 0.0f, plan, postResolve, layers, exclude);
        return plan.GetSegmentCount();
    };

    // The ordinary camera view draws all three; an environment capture fed by the third leaves out
    // the display and the surface it feeds, as its mesh gather would.
    CHECK(gathered(AllRenderLayers, Entity::Null) == 3);
    CHECK(gathered(DefaultEnvironmentCaptureLayers, excluded) == 1);
}

TEST_CASE("The per-frame record budget is shared across both placements")
{
    const RibbonScene fixture;
    vector<vec3> points;
    for (u32 i = 0; i < Renderer::MaxRibbonSegmentsPerFrame; ++i)
    {
        points.emplace_back(0.01f * static_cast<f32>(i), 0.0f, 0.0f);
    }
    const auto segments = static_cast<u32>(points.size() - 1);
    fixture.AddPath({RibbonStrip{.Points = points}}, Transform{}, true, RibbonPlacement::Scene);
    fixture.AddPath({RibbonStrip{.Points = points}}, Transform{}, true,
                    RibbonPlacement::PostResolve);

    Renderer::RibbonDrawPlan postResolve;
    const Renderer::RibbonDrawPlan scene = fixture.Gather(0.0f, postResolve);
    CHECK(scene.GetSegmentCount() + postResolve.GetSegmentCount() ==
          Renderer::MaxRibbonSegmentsPerFrame);
    CHECK(scene.Dropped + postResolve.Dropped ==
          2 * segments - Renderer::MaxRibbonSegmentsPerFrame);
}

TEST_CASE("An unoccluded path's records go to its placement's unoccluded sets, alpha still sorted")
{
    const vector<vec3> line{vec3(0.0f, 0.0f, -2.0f), vec3(1.0f, 0.0f, -2.0f)};
    for (const RibbonPlacement placement : {RibbonPlacement::Scene, RibbonPlacement::PostResolve})
    {
        const RibbonScene fixture;
        fixture.AddPath({RibbonStrip{.Points = line}}, Transform{}, /*additive=*/true, placement);
        fixture.AddPath({RibbonStrip{.Points = line}}, Transform{}, /*additive=*/false, placement);
        const Entity far = fixture.AddPath({RibbonStrip{.Points = line}},
                                           Transform{.Position = vec3(0.0f, 0.0f, -5.0f)},
                                           /*additive=*/false, placement);
        const Entity near = fixture.AddPath({RibbonStrip{.Points = line}}, Transform{},
                                            /*additive=*/false, placement);
        const Entity glow = fixture.AddPath({RibbonStrip{.Points = line}}, Transform{},
                                            /*additive=*/true, placement);
        for (const Entity unoccluded : {far, near, glow})
        {
            fixture.World->Get<RibbonPath>(unoccluded).Occluded = false;
        }

        Renderer::RibbonDrawPlan postResolve;
        const Renderer::RibbonDrawPlan scene = fixture.Gather(0.0f, postResolve);
        const Renderer::RibbonDrawPlan& drawn =
            placement == RibbonPlacement::Scene ? scene : postResolve;
        CAPTURE(static_cast<int>(placement));
        CHECK(drawn.Additive.size() == 1);
        CHECK(drawn.Alpha.size() == 1);
        CHECK(drawn.UnoccludedAdditive.size() == 1);
        REQUIRE(drawn.UnoccludedAlpha.size() == 2);
        // Farthest first, as the occluded alpha set is.
        CHECK(drawn.UnoccludedAlpha[0].Start.z < drawn.UnoccludedAlpha[1].Start.z);
        CHECK(drawn.GetSegmentCount() == 5);
    }
}
