// Math::ExpApproach: the frame-rate-independent exponential approach. Pure CPU math, no Context.
// The properties pinned here are convergence (repeated stepping lands on the target), the
// zero-speed no-op, and frame-rate independence — two half-steps land at (very nearly) the same
// value as one full step, the defining property that keeps eased motion identical across frame
// rates.

#include <doctest/doctest.h>

#include <cmath>

#include <Veng/Math/Ease.h>

using namespace Veng;

TEST_CASE("math_ease: ExpApproach converges to the target under repeated stepping")
{
    f32 value = 0.0f;
    for (int i = 0; i < 2000; ++i)
    {
        value = ExpApproach(value, 10.0f, 1.0f / 60.0f, 12.0f);
    }
    CHECK(value == doctest::Approx(10.0f));
}

TEST_CASE("math_ease: ExpApproach with zero speed is a no-op")
{
    // 1 - exp(0) == 0, so the mix stays at current regardless of the target.
    CHECK(ExpApproach(3.0f, 99.0f, 0.5f, 0.0f) == doctest::Approx(3.0f));
}

TEST_CASE("math_ease: ExpApproach moves the right direction and stays within [current, target]")
{
    const f32 up = ExpApproach(0.0f, 1.0f, 0.1f, 5.0f);
    CHECK(up > 0.0f);
    CHECK(up < 1.0f);

    const f32 down = ExpApproach(1.0f, 0.0f, 0.1f, 5.0f);
    CHECK(down < 1.0f);
    CHECK(down > 0.0f);
}

TEST_CASE("math_ease: ExpApproach is frame-rate-independent — two half-steps match one full step")
{
    constexpr f32 target = 1.0f;
    constexpr f32 speed = 8.0f;
    constexpr f32 dt = 1.0f / 30.0f;

    const f32 oneStep = ExpApproach(0.0f, target, dt, speed);

    f32 twoHalfSteps = ExpApproach(0.0f, target, dt * 0.5f, speed);
    twoHalfSteps = ExpApproach(twoHalfSteps, target, dt * 0.5f, speed);

    // Exact in real arithmetic (the decay composes multiplicatively); float rounding leaves only a
    // tiny residue, far tighter than any perceptible motion difference.
    CHECK(twoHalfSteps == doctest::Approx(oneStep).epsilon(1e-6));
}

TEST_CASE("math_ease: ExpApproach(vec3) converges and holds on a zero speed")
{
    vec3 value(0.0f);
    const vec3 target(3.0f, -2.0f, 5.0f);
    for (int i = 0; i < 2000; ++i)
    {
        value = ExpApproach(value, target, 1.0f / 60.0f, 12.0f);
    }
    CHECK(value.x == doctest::Approx(target.x));
    CHECK(value.y == doctest::Approx(target.y));
    CHECK(value.z == doctest::Approx(target.z));

    // 1 - exp(0) == 0, so a zero speed leaves the vector untouched.
    const vec3 held = ExpApproach(vec3(1.0f, 2.0f, 3.0f), target, 0.5f, 0.0f);
    CHECK(held.x == doctest::Approx(1.0f));
    CHECK(held.y == doctest::Approx(2.0f));
    CHECK(held.z == doctest::Approx(3.0f));
}

TEST_CASE("math_ease: ExpApproach(quat) converges to the target and stays unit-length")
{
    quat value = glm::angleAxis(glm::radians(10.0f), glm::normalize(vec3(1.0f, 0.0f, 0.0f)));
    const quat target =
        glm::angleAxis(glm::radians(140.0f), glm::normalize(vec3(0.2f, 1.0f, 0.3f)));
    f32 maxUnitDrift = 0.0f;
    for (int i = 0; i < 2000; ++i)
    {
        value = ExpApproach(value, target, 1.0f / 60.0f, 12.0f);
        maxUnitDrift = glm::max(maxUnitDrift, glm::abs(glm::length(value) - 1.0f));
    }
    CHECK(maxUnitDrift < 1e-5f);
    // A quaternion and its negation are the same rotation; compare through the absolute dot.
    CHECK(glm::abs(glm::dot(value, target)) == doctest::Approx(1.0f).epsilon(1e-4));

    // Zero speed is a no-op: the orientation is returned unchanged.
    const quat held = ExpApproach(value, target, 0.5f, 0.0f);
    CHECK(glm::abs(glm::dot(held, value)) == doctest::Approx(1.0f).epsilon(1e-6));
}
