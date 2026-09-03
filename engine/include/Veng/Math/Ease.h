#pragma once

#include <Veng/Veng.h>

/// @brief Frame-rate-independent easing — pure, device-free, header-only.
///
/// A discrete step of a continuous exponential decay: sampling the same decay over two
/// half-steps lands at the same value as one full step, so motion driven by `ExpApproach`
/// looks identical at any frame rate.
namespace Veng
{
    /// @brief Exponentially approaches a target, frame-rate-independent.
    ///
    /// Moves `current` toward `target` by the fraction `1 - exp(-delta * speed)` — the
    /// closed form of a continuous exponential decay sampled over one time step. The result
    /// converges to `target` as time accumulates, and the motion is independent of how the
    /// elapsed time is subdivided: approaching over two half-steps lands at the same value as
    /// one full step. `speed` is the decay rate in units of 1/seconds — a larger speed
    /// converges faster; a zero speed holds `current` unchanged.
    /// @param current  The value moving this step.
    /// @param target   The value being approached.
    /// @param delta    The elapsed time step, in seconds.
    /// @param speed    The decay rate, in 1/seconds; a zero speed makes the step a no-op.
    /// @return The value stepped toward `target`.
    [[nodiscard]] inline f32 ExpApproach(const f32 current, const f32 target, const f32 delta,
                                         const f32 speed)
    {
        return glm::mix(current, target, 1.0f - glm::exp(-delta * speed));
    }

    /// @brief Exponentially approaches a target vector, frame-rate-independent.
    ///
    /// The component-wise vector form of the scalar overload: each component is mixed toward
    /// `target` by the shared fraction `1 - exp(-delta * speed)`, so the whole vector converges
    /// along the straight segment to `target` with the same frame-rate independence. `speed` is
    /// the decay rate in units of 1/seconds; a zero speed holds `current` unchanged.
    /// @param current  The vector moving this step.
    /// @param target   The vector being approached.
    /// @param delta    The elapsed time step, in seconds.
    /// @param speed    The decay rate, in 1/seconds; a zero speed makes the step a no-op.
    /// @return The vector stepped toward `target`.
    [[nodiscard]] inline vec3 ExpApproach(const vec3& current, const vec3& target, const f32 delta,
                                          const f32 speed)
    {
        return glm::mix(current, target, 1.0f - glm::exp(-delta * speed));
    }

    /// @brief Exponentially approaches a target orientation, frame-rate-independent.
    ///
    /// The rotational form of the scalar overload: `slerp`s `current` toward `target` by the
    /// fraction `1 - exp(-delta * speed)` and renormalizes, so orientation damps toward `target`
    /// along the shortest arc with the same frame-rate independence — two half-steps land at (very
    /// nearly) the same rotation as one full step. `speed` is the decay rate in 1/seconds; a zero
    /// speed holds `current` unchanged.
    /// @param current  The orientation moving this step; assumed unit-length.
    /// @param target   The orientation being approached; assumed unit-length.
    /// @param delta    The elapsed time step, in seconds.
    /// @param speed    The decay rate, in 1/seconds; a zero speed makes the step a no-op.
    /// @return The unit-length orientation stepped toward `target`.
    [[nodiscard]] inline quat ExpApproach(const quat& current, const quat& target, const f32 delta,
                                          const f32 speed)
    {
        return glm::normalize(glm::slerp(current, target, 1.0f - glm::exp(-delta * speed)));
    }
}
