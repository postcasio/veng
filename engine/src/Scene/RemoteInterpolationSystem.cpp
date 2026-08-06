#include <Veng/Scene/RemoteInterpolationSystem.h>

#include <Veng/Math/Ease.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace Veng
{
    namespace
    {
        // A predicted entity is simulated locally, not interpolated — the interpolation system skips
        // it so a buffered past sample never overwrites the live, client-driven pose.
        bool IsPredicted(const Scene& scene, const Entity entity)
        {
            const Authority* authority = scene.TryGet<Authority>(entity);
            return authority != nullptr && authority->Tier == Tier::Predicted;
        }

        // The render-residual decay rate: ~1/s so a correction eases out over ~150 ms
        // (exp(-20 * 0.15) ≈ 5% remaining), frame-rate-independent through ExpApproach.
        constexpr f32 PredictionSmoothingSpeed = 20.0f;

        // Below these the residual is imperceptible and its component is dropped.
        constexpr f32 NegligiblePosition = 1.0e-4f; // meters
        constexpr f32 NegligibleRotation = 1.0e-6f; // 1 - |dot(q, identity)|
    }

    PredictionError DecayPredictionError(const PredictionError& error, const f32 delta,
                                         const f32 speed)
    {
        const quat identity{1.0f, 0.0f, 0.0f, 0.0f};
        const f32 t = 1.0f - glm::exp(-delta * speed);
        return PredictionError{
            .Position = glm::mix(error.Position, vec3(0.0f), t),
            .Rotation = glm::normalize(glm::slerp(error.Rotation, identity, t)),
        };
    }

    bool IsPredictionErrorNegligible(const PredictionError& error)
    {
        const quat identity{1.0f, 0.0f, 0.0f, 0.0f};
        const f32 rotationDrift = 1.0f - std::abs(glm::dot(error.Rotation, identity));
        return glm::length(error.Position) < NegligiblePosition &&
               rotationDrift < NegligibleRotation;
    }

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
        // Decay the render residual of every corrected predicted entity toward zero — the visual
        // ease-out of a reconciliation snap, applied only at the gather (never into Transform). Runs
        // before the remote-interpolation clock so it advances even for a client with no remote
        // mirrors (its own predicted pawn alone). The mutable Transform access bumps the scene's
        // spatial version so the render gather re-reads the shrinking offset each frame; the Transform
        // value itself is untouched. A settled residual removes its component after the walk.
        {
            vector<Entity> settled;
            for (auto [entity, transform, error] : scene.View<Transform, PredictionError>())
            {
                (void)transform;
                error = DecayPredictionError(error, delta, PredictionSmoothingSpeed);
                if (IsPredictionErrorNegligible(error))
                {
                    settled.push_back(entity);
                }
            }
            for (const Entity entity : settled)
            {
                (void)scene.Remove<PredictionError>(entity);
            }
        }

        // The newest received tick across every remote buffer anchors the shared playback timeline.
        u64 newest = 0;
        bool anySamples = false;
        for (auto [entity, interp] : scene.View<const RemoteInterpolation>())
        {
            if (IsPredicted(scene, entity))
            {
                continue;
            }
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
            [&scene, renderTick](const Entity entity, Transform& transform,
                                 RemoteInterpolation& interp)
            {
                if (IsPredicted(scene, entity))
                {
                    return;
                }
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
