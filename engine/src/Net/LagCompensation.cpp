#include <Veng/Net/LagCompensation.h>

#include <Veng/Assert.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Veng
{
    namespace
    {
        RewoundBodyPose FromSample(const PoseSample& sample)
        {
            return RewoundBodyPose{.Pose = sample.Pose,
                                   .LinearVelocity = sample.LinearVelocity,
                                   .AngularVelocity = sample.AngularVelocity};
        }

        RewoundBodyPose Blend(const PoseSample& lo, const PoseSample& hi, const f64 alpha)
        {
            const auto a = static_cast<f32>(alpha);
            return RewoundBodyPose{
                .Pose = PhysicsPose{.Position = glm::mix(lo.Pose.Position, hi.Pose.Position, alpha),
                                    .Rotation = glm::slerp(lo.Pose.Rotation, hi.Pose.Rotation, a)},
                .LinearVelocity = glm::mix(lo.LinearVelocity, hi.LinearVelocity, a),
                .AngularVelocity = glm::mix(lo.AngularVelocity, hi.AngularVelocity, a),
            };
        }
    }

    void PoseHistory::SetSettings(const Settings& settings)
    {
        m_Settings = settings;
        TrimAll();
    }

    void PoseHistory::SetSimTickRate(const f64 ticksPerSecond)
    {
        VE_ASSERT(ticksPerSecond > 0.0, "PoseHistory: the sim tick rate must be positive");
        m_SimTickRate = ticksPerSecond;
        TrimAll();
    }

    u64 PoseHistory::GetMaxRewindTicks() const
    {
        // Rounded down, so the reach never exceeds MaxRewindSeconds; the epsilon keeps an exact
        // product (0.25 s at 60 Hz) from flooring a tick short.
        const f64 ticks =
            std::floor(std::max(m_Settings.MaxRewindSeconds, 0.0) * m_SimTickRate + 1e-6);
        return static_cast<u64>(ticks);
    }

    void PoseHistory::Record(const Entity entity, const PoseSample& sample)
    {
        vector<PoseSample>& ring = m_Rings[entity];
        if (!ring.empty())
        {
            VE_ASSERT(sample.Tick >= ring.back().Tick,
                      "PoseHistory: tick {} recorded after tick {}", sample.Tick, ring.back().Tick);
            if (sample.Tick == ring.back().Tick)
            {
                ring.back() = sample;
                return;
            }
        }
        ring.push_back(sample);
        Trim(ring);
    }

    void PoseHistory::EndTick(const u64 tick)
    {
        std::erase_if(m_Rings, [tick](const auto& entry)
                      { return entry.second.empty() || entry.second.back().Tick != tick; });
        m_NewestTick = tick;
    }

    void PoseHistory::Clear()
    {
        m_Rings.clear();
        m_NewestTick.reset();
    }

    f64 PoseHistory::ClampViewTick(const f64 viewTick) const
    {
        if (!m_NewestTick)
        {
            return viewTick;
        }
        const auto newest = static_cast<f64>(*m_NewestTick);
        const f64 oldest = std::max(newest - static_cast<f64>(GetMaxRewindTicks()), 0.0);
        return std::clamp(viewTick, oldest, newest);
    }

    optional<RewoundBodyPose> PoseHistory::RewoundPose(const Entity entity,
                                                       const f64 viewTick) const
    {
        const auto found = m_Rings.find(entity);
        if (found == m_Rings.end() || found->second.empty())
        {
            return std::nullopt;
        }
        const vector<PoseSample>& ring = found->second;

        // Hold at the ends, as a client's interpolation holds on a stalled or still-filling buffer.
        if (viewTick <= static_cast<f64>(ring.front().Tick))
        {
            return FromSample(ring.front());
        }
        if (viewTick >= static_cast<f64>(ring.back().Tick))
        {
            return FromSample(ring.back());
        }

        // A client received samples only at snapshot ticks, so only those (and the ring's own ends,
        // for a view near the edge of the reach) bracket the blend.
        const u64 interval = m_Settings.SnapshotInterval;
        const auto isCandidate = [&](const usize i)
        { return i + 1 == ring.size() || interval == 0 || ring[i].Tick % interval == 0; };

        usize lo = 0;
        for (usize i = 1; i < ring.size(); ++i)
        {
            if (!isCandidate(i))
            {
                continue;
            }
            if (static_cast<f64>(ring[i].Tick) >= viewTick)
            {
                const auto loTick = static_cast<f64>(ring[lo].Tick);
                const f64 span = static_cast<f64>(ring[i].Tick) - loTick;
                return Blend(ring[lo], ring[i], (viewTick - loTick) / span);
            }
            lo = i;
        }
        return FromSample(ring.back());
    }

    std::span<const PoseSample> PoseHistory::GetSamples(const Entity entity) const
    {
        const auto found = m_Rings.find(entity);
        return found != m_Rings.end() ? std::span<const PoseSample>(found->second)
                                      : std::span<const PoseSample>();
    }

    void PoseHistory::GetEntities(vector<Entity>& out) const
    {
        out.clear();
        out.reserve(m_Rings.size());
        for (const auto& [entity, ring] : m_Rings)
        {
            out.push_back(entity);
        }
        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
    }

    void PoseHistory::Trim(vector<PoseSample>& ring) const
    {
        if (ring.empty())
        {
            return;
        }
        const u64 reach = GetMaxRewindTicks();
        const u64 newest = ring.back().Tick;
        const u64 oldest = newest > reach ? newest - reach : 0;
        const auto keep = std::ranges::find_if(ring, [oldest](const PoseSample& sample)
                                               { return sample.Tick >= oldest; });
        ring.erase(ring.begin(), keep);
    }

    void PoseHistory::TrimAll()
    {
        for (auto& [entity, ring] : m_Rings)
        {
            Trim(ring);
        }
    }

    PoseHistory& EnsurePoseHistory(Scene& scene, const PoseHistory::Settings& settings)
    {
        PoseHistory* history = scene.GetPoseHistory();
        if (history == nullptr)
        {
            scene.SetPoseHistory(CreateUnique<PoseHistory>(settings));
            return *scene.GetPoseHistory();
        }
        if (!(history->GetSettings() == settings))
        {
            history->SetSettings(settings);
        }
        return *history;
    }

    optional<RewoundBodyPose> RewoundPose(const Scene& scene, const Entity entity,
                                          const f64 viewTick)
    {
        const PoseHistory* history = scene.GetPoseHistory();
        return history != nullptr ? history->RewoundPose(entity, viewTick) : std::nullopt;
    }

    void PoseHistorySystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        // The history is the server's judgement of what a client saw; a client neither judges nor
        // replays anything against it.
        if (context.Role != NetRole::Server || context.IsReplay)
        {
            return;
        }

        // Read through a const view so the walk never bumps the spatial version.
        const Scene& reader = scene;
        const PhysicsWorld* world = scene.GetPhysicsWorld();
        PoseHistory* history = scene.GetPoseHistory();
        if (history == nullptr)
        {
            if (world == nullptr || reader.TryGetFirst<LagCompensated>() == nullptr)
            {
                return;
            }
            history = &EnsurePoseHistory(scene, PoseHistory::Settings{});
        }

        // The step's delta is the world's own rate; rounding keeps an f32 1/60 from reading as a
        // rate a hair above 60, which would floor the reach a tick long.
        if (delta > 0.0f)
        {
            const f64 rate = std::round(1.0 / static_cast<f64>(delta));
            if (rate > 0.0 && rate != history->GetSimTickRate())
            {
                history->SetSimTickRate(rate);
            }
        }

        if (world != nullptr)
        {
            reader.Each<LagCompensated>(
                [&](const Entity entity, const LagCompensated&)
                {
                    const optional<PhysicsPose> pose = world->GetBodyPose(entity);
                    if (!pose)
                    {
                        return;
                    }
                    history->Record(
                        entity, PoseSample{.Tick = context.Tick,
                                           .Pose = *pose,
                                           .LinearVelocity = world->GetLinearVelocity(entity),
                                           .AngularVelocity = world->GetAngularVelocity(entity)});
                });
        }
        history->EndTick(context.Tick);
    }

    RewindScope::RewindScope(const Scene& scene, PhysicsWorld& world, const f64 viewTick,
                             const Entity exclude)
        : m_World(&world), m_StepCount(world.GetStepCount()), m_ViewTick(viewTick)
    {
        const PoseHistory* history = scene.GetPoseHistory();
        if (history == nullptr || !history->GetNewestTick())
        {
            return;
        }
        m_ViewTick = history->ClampViewTick(viewTick);
        if (m_ViewTick >= static_cast<f64>(*history->GetNewestTick()))
        {
            return;
        }

        vector<Entity> recorded;
        history->GetEntities(recorded);
        m_Moved.reserve(recorded.size());
        for (const Entity entity : recorded)
        {
            if (entity == exclude || !scene.IsAlive(entity) || !scene.Has<LagCompensated>(entity) ||
                !world.HasBody(entity))
            {
                continue;
            }
            const optional<RewoundBodyPose> past = history->RewoundPose(entity, m_ViewTick);
            if (!past)
            {
                continue;
            }
            m_Moved.push_back(MovedBody{.Body = entity, .State = world.SaveBodyState(entity)});
            world.SetBodyPose(entity, past->Pose, BodyActivation::Keep);
        }
    }

    RewindScope::~RewindScope()
    {
        VE_ASSERT(m_World->GetStepCount() == m_StepCount,
                  "RewindScope held across a physics step: the step simulated rewound poses");
        for (auto it = m_Moved.rbegin(); it != m_Moved.rend(); ++it)
        {
            const VoidResult restored = m_World->RestoreBodyState(it->Body, it->State);
            VE_ASSERT(restored.has_value(), "RewindScope could not restore a body: {}",
                      restored.error());
        }
    }
}
