#include <Veng/Scene/RemoteInterpolationSystem.h>

#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>

namespace Veng
{
    optional<Transform> SampleRemoteInterpolation(const std::span<const RemoteSample> samples,
                                                  const f64 renderTick)
    {
        if (samples.empty())
        {
            return std::nullopt;
        }

        const auto poseOf = [](const RemoteSample& sample)
        {
            return Transform{
                .Position = sample.Position, .Rotation = sample.Rotation, .Scale = sample.Scale};
        };

        // Hold at the ends: before the oldest or after the newest sample, show that boundary pose
        // rather than extrapolating (a stalled or still-filling buffer shows the nearest real pose).
        if (renderTick <= static_cast<f64>(samples.front().ServerTick))
        {
            return poseOf(samples.front());
        }
        if (renderTick >= static_cast<f64>(samples.back().ServerTick))
        {
            return poseOf(samples.back());
        }

        // Find the first sample strictly newer than renderTick; the bracket is [prev, it].
        for (usize i = 1; i < samples.size(); ++i)
        {
            const RemoteSample& hi = samples[i];
            if (static_cast<f64>(hi.ServerTick) >= renderTick)
            {
                const RemoteSample& lo = samples[i - 1];
                const f64 span = static_cast<f64>(hi.ServerTick) - static_cast<f64>(lo.ServerTick);
                // Equal ticks (a degenerate pair) resolve to the newer sample rather than dividing by zero.
                const f32 alpha =
                    span > 0.0
                        ? static_cast<f32>((renderTick - static_cast<f64>(lo.ServerTick)) / span)
                        : 1.0f;
                return InterpolateTransform(poseOf(lo), poseOf(hi), alpha);
            }
        }

        // Unreachable given the newest-hold above, but keep the newest pose as a total fallback.
        return poseOf(samples.back());
    }

    void RemoteInterpolationSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext&)
    {
        // The newest received tick across every remote buffer anchors the shared playback timeline.
        u64 newest = 0;
        bool anySamples = false;
        for (auto [entity, interp] : scene.View<const RemoteInterpolation>())
        {
            (void)entity;
            if (!interp.Samples.empty())
            {
                anySamples = true;
                newest = std::max(newest, interp.Samples.back().ServerTick);
            }
        }
        if (!anySamples)
        {
            // No remote state yet: idle without seeding the clock, so it starts at the first samples.
            return;
        }

        const f64 delayTicks = static_cast<f64>(m_Settings.InterpolationDelayIntervals) *
                               static_cast<f64>(m_Settings.SnapshotInterval);
        const f64 target = static_cast<f64>(newest) - delayTicks;

        if (!m_Initialized)
        {
            m_PlaybackTick = target;
            m_Initialized = true;
        }
        else
        {
            m_PlaybackTick += static_cast<f64>(delta) * m_Settings.SimTickRate;
        }

        // Keep the clock inside the received window: never ahead of the newest sample (no
        // extrapolation), and snap forward if it has fallen more than the delay behind the target
        // (a hitch or a fresh burst of samples after a gap).
        if (m_PlaybackTick > static_cast<f64>(newest))
        {
            m_PlaybackTick = static_cast<f64>(newest);
        }
        if (m_PlaybackTick < target - delayTicks)
        {
            m_PlaybackTick = target;
        }

        const f64 renderTick = m_PlaybackTick;

        scene.Each<Transform, RemoteInterpolation>(
            [renderTick](const Entity, Transform& transform, RemoteInterpolation& interp)
            {
                const optional<Transform> pose =
                    SampleRemoteInterpolation(interp.Samples, renderTick);
                if (pose.has_value())
                {
                    transform = *pose;
                }

                // Drop samples fully behind the render tick, keeping the one immediately before it so
                // the bracket stays intact. Bounds the buffer without losing the active interpolation.
                usize keepFrom = 0;
                for (usize i = 0; i + 1 < interp.Samples.size(); ++i)
                {
                    if (static_cast<f64>(interp.Samples[i + 1].ServerTick) <= renderTick)
                    {
                        keepFrom = i + 1;
                    }
                    else
                    {
                        break;
                    }
                }
                if (keepFrom > 0)
                {
                    interp.Samples.erase(interp.Samples.begin(),
                                         interp.Samples.begin() + static_cast<isize>(keepFrom));
                }
            });
    }
}
