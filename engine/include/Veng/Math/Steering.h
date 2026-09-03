#pragma once

#include <Veng/Veng.h>

/// @brief Pure, device-free steering arithmetic — turning a goal into rate commands.
///
/// The functions here convert an autonomous mover's goal — a point to reach, a direction to
/// face, a moving frame to match velocity with — into the commands a mover integrates: a desired
/// velocity, and a per-axis body-frame rotation for this tick. They know nothing of vehicles,
/// components, or the scene; they are the arithmetic an `Intent`-writing controller composes.
///
/// Conventions, stated once and shared by every function below:
/// - **Forward is -Z, up is +Y, right is +X** — the engine's camera/socket body-frame basis, and
///   the frame `FacingRates`' output is expressed in.
/// - **Rotational output is radians for this tick**, per axis, already scaled by `delta` and
///   clamped — directly assignable to a mover whose look command is radians-per-tick.
/// - **Positions and velocities are frame-agnostic:** all vector inputs to one call live in a
///   single caller-chosen reference frame, and the result comes back in that same frame. The
///   functions never assume world space.
namespace Veng
{
    /// @brief The fastest speed from which a constant deceleration still stops within a distance.
    ///
    /// The `sqrt(2 * deceleration * distance)` braking curve, capped at `maxSpeed`: the largest
    /// speed a mover may hold now so that decelerating at `deceleration` brings it to rest by the
    /// time it has travelled `distance`. Monotone non-decreasing in `distance`, and never above
    /// `maxSpeed`. A non-positive `deceleration` means "no braking", so the cap is returned
    /// unchanged; a negative `distance` is treated as zero.
    /// @param distance      The remaining distance over which to stop.
    /// @param maxSpeed      The upper bound on the returned speed.
    /// @param deceleration  The available constant deceleration, in distance per second squared.
    /// @return The braking-limited speed, in `[0, maxSpeed]`.
    [[nodiscard]] inline f32 ArriveSpeed(const f32 distance, const f32 maxSpeed,
                                         const f32 deceleration)
    {
        if (deceleration <= 0.0f)
        {
            return maxSpeed;
        }
        const f32 clamped = glm::max(distance, 0.0f);
        return glm::min(maxSpeed, glm::sqrt(2.0f * deceleration * clamped));
    }

    /// @brief The desired velocity that steers toward a point and brakes to a stop at it.
    ///
    /// Points at `target` from `position` and scales by `ArriveSpeed` over the distance remaining
    /// to the stop band, so the mover runs at `maxSpeed` far out and eases to zero as it reaches
    /// `stopRadius` of the target. Inside `stopRadius` (or at the target) the desired velocity is
    /// zero.
    /// @param position      The mover's current position.
    /// @param target        The point to arrive at.
    /// @param maxSpeed      The cruising speed held while far from the target.
    /// @param deceleration  The braking deceleration used to size the approach ramp.
    /// @param stopRadius    The radius around the target within which the desired velocity is zero.
    /// @return The desired velocity, in the shared frame; zero inside `stopRadius`.
    [[nodiscard]] inline vec3 Arrive(const vec3& position, const vec3& target, const f32 maxSpeed,
                                     const f32 deceleration, const f32 stopRadius)
    {
        const vec3 offset = target - position;
        const f32 distance = glm::length(offset);
        if (distance <= stopRadius || distance <= 1e-6f)
        {
            return vec3(0.0f);
        }
        const f32 speed = ArriveSpeed(distance - stopRadius, maxSpeed, deceleration);
        return offset * (speed / distance);
    }

    /// @brief The desired velocity that steers toward a point at full speed, without braking.
    ///
    /// The degenerate `Arrive` with no deceleration ramp: full `maxSpeed` straight at `target`
    /// until the mover is essentially on it, then zero. Kept as its own name so a caller's
    /// behaviour tree reads as "seek" where no arrival easing is wanted.
    /// @param position  The mover's current position.
    /// @param target    The point to steer toward.
    /// @param maxSpeed  The speed to steer at.
    /// @return The desired velocity, in the shared frame; zero at the target.
    [[nodiscard]] inline vec3 Seek(const vec3& position, const vec3& target, const f32 maxSpeed)
    {
        const vec3 offset = target - position;
        const f32 distance = glm::length(offset);
        if (distance <= 1e-6f)
        {
            return vec3(0.0f);
        }
        return offset * (maxSpeed / distance);
    }

    /// @brief The desired absolute velocity that arrives at a point which is itself moving.
    ///
    /// Runs `Arrive` in the target's frame — steering toward `targetPosition` with the relative
    /// speed capped at `maxRelativeSpeed` — and adds `targetVelocity` back, so the returned
    /// absolute velocity closes the gap while matching the target's motion. As the mover reaches
    /// the stop band the relative term eases to zero and the result converges on `targetVelocity`,
    /// which is what lets a mover settle onto a point carried by a moving structure.
    /// @param position          The mover's current position.
    /// @param velocity          The mover's current absolute velocity.
    /// @param targetPosition    The moving point to arrive at.
    /// @param targetVelocity    The velocity of that point.
    /// @param maxRelativeSpeed  The cap on the closing speed relative to the target.
    /// @param deceleration      The braking deceleration used to size the approach ramp.
    /// @param stopRadius        The radius within which the relative closing velocity is zero.
    /// @return The desired absolute velocity, in the shared frame.
    [[nodiscard]] inline vec3
    ApproachMovingPoint(const vec3& position, [[maybe_unused]] const vec3& velocity,
                        const vec3& targetPosition, const vec3& targetVelocity,
                        const f32 maxRelativeSpeed, const f32 deceleration, const f32 stopRadius)
    {
        return targetVelocity +
               Arrive(position, targetPosition, maxRelativeSpeed, deceleration, stopRadius);
    }

    /// @brief The unsigned angle between two vectors, in radians.
    ///
    /// Robust to un-normalized inputs and clamped against floating-point drift past `±1`, so the
    /// result is always a real number in `[0, pi]`. A zero-length input has no direction, so the
    /// angle is zero.
    /// @param a  The first vector.
    /// @param b  The second vector.
    /// @return The angle between the two directions, in `[0, pi]` radians.
    [[nodiscard]] inline f32 AngleBetween(const vec3& a, const vec3& b)
    {
        const f32 lengths = glm::length(a) * glm::length(b);
        if (lengths <= 1e-12f)
        {
            return 0.0f;
        }
        return glm::acos(glm::clamp(glm::dot(a, b) / lengths, -1.0f, 1.0f));
    }

    /// @brief The shortest-arc rotation taking one direction onto another.
    ///
    /// The unit quaternion that rotates `from` onto `to` about their common perpendicular, the
    /// smaller of the two possible turns. Inputs are normalized internally. Two degenerate cases
    /// are handled explicitly: already-aligned directions return the identity, and exactly opposed
    /// directions return a half turn about an arbitrary axis perpendicular to `from` (any such axis
    /// is equally correct). A zero-length input returns the identity.
    /// @param from  The starting direction.
    /// @param to    The direction to rotate onto.
    /// @return The unit-length rotation carrying `from` onto `to`.
    [[nodiscard]] inline quat ShortestArc(const vec3& from, const vec3& to)
    {
        const f32 lengthFrom = glm::length(from);
        const f32 lengthTo = glm::length(to);
        if (lengthFrom <= 1e-6f || lengthTo <= 1e-6f)
        {
            return quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        const vec3 f = from / lengthFrom;
        const vec3 t = to / lengthTo;
        const f32 cosAngle = glm::dot(f, t);
        if (cosAngle >= 1.0f - 1e-6f)
        {
            return quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        if (cosAngle <= -1.0f + 1e-6f)
        {
            // Antiparallel: any axis perpendicular to `f` gives a valid half turn. Cross with the
            // world basis vector `f` is least parallel to, so the cross is well-conditioned.
            vec3 axis = glm::cross(vec3(1.0f, 0.0f, 0.0f), f);
            if (glm::dot(axis, axis) < 1e-12f)
            {
                axis = glm::cross(vec3(0.0f, 1.0f, 0.0f), f);
            }
            return glm::angleAxis(glm::radians(180.0f), glm::normalize(axis));
        }
        const vec3 axis = glm::normalize(glm::cross(f, t));
        return glm::angleAxis(glm::acos(cosAngle), axis);
    }

    /// @brief The per-axis body-frame rotation, this tick, that turns toward a desired facing.
    ///
    /// Produces the yaw, pitch and roll (radians, this tick) that rotate `orientation`'s forward
    /// (-Z) toward `desiredForward` and its up (+Y) toward `desiredUp`. Yaw and pitch aim the
    /// forward axis along the shortest arc — a pure turn with no twist about forward — and roll then
    /// spins about that forward axis to bring up onto `desiredUp`. Each axis error is turned into a
    /// rate by `gain`, capped per axis to `maxRates`, and integrated over `delta`, so a far-off
    /// target turns at the axis rate cap and a near-aligned one eases in. Integrated by a mover as
    /// yaw about local up, pitch about local right, and roll about local forward, the output drives
    /// the heading error monotonically to zero — the shortest-arc aim is what keeps the heading
    /// falling even while a roll command runs.
    ///
    /// A zero-length `desiredUp` means "any up": roll is left at zero and only yaw and pitch are
    /// produced. A zero-length `desiredForward` has no facing to aim at, so all three are zero. The
    /// aligned and exactly-opposed extremes are finite (no division by a vanishing axis). When
    /// `desiredUp` is not perpendicular to `desiredForward`, up settles on its projection into the
    /// plane perpendicular to forward — the closest an up axis can come to it.
    /// @param orientation     The mover's current orientation; assumed unit-length.
    /// @param desiredForward  The direction the forward axis should point, in the shared frame.
    /// @param desiredUp       The direction the up axis should point; zero-length for "any up".
    /// @param maxRates        The per-axis turn-rate caps (x yaw, y pitch, z roll), in radians/second.
    /// @param gain            The proportional rate gain, in 1/seconds, mapping angular error to a
    ///                        turn rate before the per-axis cap.
    /// @param delta           The tick duration, in seconds, the capped rate is integrated over.
    /// @return The body-frame command `{ yaw, pitch, roll }` in radians for this tick.
    [[nodiscard]] inline vec3 FacingRates(const quat& orientation, const vec3& desiredForward,
                                          const vec3& desiredUp, const vec3& maxRates,
                                          const f32 gain, const f32 delta)
    {
        const f32 forwardLength = glm::length(desiredForward);
        if (forwardLength <= 1e-6f)
        {
            return vec3(0.0f);
        }

        // Work in the body frame, where the current forward is (0,0,-1) and up is (0,1,0), so each
        // axis error is a plain angle. Yaw is about local up (+Y), pitch about local right (+X),
        // roll about local forward (-Z) — the axes a mover integrates the result on.
        const quat toBody = glm::conjugate(orientation);
        const vec3 forwardLocal = (toBody * desiredForward) / forwardLength;

        // Aim the forward axis along the shortest arc and read off its yaw/pitch components. The
        // arc's axis is perpendicular to forward, so it carries no roll (twist) term: a roll
        // command never disturbs the heading, which is what makes the heading error fall monotone.
        const quat aim = ShortestArc(vec3(0.0f, 0.0f, -1.0f), forwardLocal);
        const f32 sinHalf = glm::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
        vec3 aimError(0.0f);
        if (sinHalf > 1e-6f)
        {
            aimError = vec3(aim.x, aim.y, aim.z) * (2.0f * glm::atan(sinHalf, aim.w) / sinHalf);
        }
        const f32 yawError = aimError.y;
        const f32 pitchError = aimError.x;

        // Roll about local forward (-Z) rolls the up axis onto the desired up, measured in the
        // plane perpendicular to forward. Absent a desired up there is nothing to roll toward.
        f32 rollError = 0.0f;
        const f32 upLength = glm::length(desiredUp);
        if (upLength > 1e-6f)
        {
            const vec3 upLocal = (toBody * desiredUp) / upLength;
            rollError = glm::atan(upLocal.x, upLocal.y);
        }

        // Proportional turn rate, capped per axis, then integrated over the tick.
        const vec3 rate =
            glm::clamp(gain * vec3(yawError, pitchError, rollError), -maxRates, maxRates);
        return rate * delta;
    }
}
