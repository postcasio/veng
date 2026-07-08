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
