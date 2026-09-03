// Veng::Steering: pure, device-free arithmetic turning a goal into rate commands. No Context.
// The properties pinned here are the ones a mover relies on: the arrive braking curve is monotone,
// bounded, and stops a Euler-integrated body inside its stop radius; a matched-frame approach to a
// point on a moving circle arrives slow; and the facing law drives the forward error monotonically
// to zero while never exceeding its per-axis rate caps.

#include <doctest/doctest.h>

#include <cmath>

#include <Veng/Math/Steering.h>

using namespace Veng;

namespace
{
    // Applies one tick of FacingRates output the way the built-in mover integrates it: yaw about
    // local up (+Y), pitch about local right (+X), roll about local forward (-Z), body-local.
    quat IntegrateFacing(const quat& orientation, const vec3& rates)
    {
        const quat step = glm::angleAxis(rates.x, vec3(0.0f, 1.0f, 0.0f)) *
                          glm::angleAxis(rates.y, vec3(1.0f, 0.0f, 0.0f)) *
                          glm::angleAxis(rates.z, vec3(0.0f, 0.0f, -1.0f));
        return glm::normalize(orientation * step);
    }
}

TEST_CASE("math_steering: ArriveSpeed is monotone in distance and capped at maxSpeed")
{
    constexpr f32 maxSpeed = 50.0f;
    constexpr f32 deceleration = 10.0f;

    bool nonDecreasing = true;
    f32 maxObserved = 0.0f;
    f32 previous = -1.0f;
    for (int i = 0; i <= 400; ++i)
    {
        const f32 distance = static_cast<f32>(i) * 0.5f;
        const f32 speed = ArriveSpeed(distance, maxSpeed, deceleration);
        nonDecreasing = nonDecreasing && speed >= previous - 1e-5f;
        maxObserved = glm::max(maxObserved, speed);
        previous = speed;
    }
    CHECK(nonDecreasing);
    CHECK(maxObserved <= maxSpeed + 1e-4f);
    CHECK(maxObserved == doctest::Approx(maxSpeed)); // far enough out the cap binds

    // A negative distance clamps to zero, a non-positive deceleration means "no braking".
    CHECK(ArriveSpeed(-5.0f, maxSpeed, deceleration) == doctest::Approx(0.0f));
    CHECK(ArriveSpeed(3.0f, maxSpeed, 0.0f) == doctest::Approx(maxSpeed));
}

TEST_CASE("math_steering: Arrive brakes a body to rest inside the stop radius without overshoot")
{
    constexpr f32 maxSpeed = 40.0f;
    constexpr f32 deceleration = 2.0f;
    constexpr f32 stopRadius = 0.5f;
    constexpr f32 dt = 1.0f / 60.0f;
    const f32 stepCap = maxSpeed * dt; // the most a body can travel in one tick

    f32 worstFinalDistance = 0.0f;
    f32 worstOvershoot = 0.0f;
    bool allStopped = true;
    for (const f32 start : {5.0f, 20.0f, 50.0f, 100.0f, 200.0f})
    {
        f32 pos = start; // 1-D: target at the origin, body approaching from +x
        f32 minPos = start;
        f32 lastSpeed = 0.0f;
        for (int step = 0; step < 4000; ++step)
        {
            const vec3 velocity =
                Arrive(vec3(pos, 0.0f, 0.0f), vec3(0.0f), maxSpeed, deceleration, stopRadius);
            lastSpeed = glm::abs(velocity.x);
            pos += velocity.x * dt;
            minPos = glm::min(minPos, pos);
            if (lastSpeed == 0.0f)
            {
                break;
            }
        }
        allStopped = allStopped && lastSpeed == 0.0f;
        worstFinalDistance = glm::max(worstFinalDistance, glm::abs(pos));
        worstOvershoot = glm::max(worstOvershoot, glm::max(0.0f, -minPos));
    }
    CHECK(allStopped);
    CHECK(worstFinalDistance <= stopRadius + 1e-4f);
    CHECK(worstOvershoot <= stepCap); // never past the target by more than one tick's travel
}

TEST_CASE("math_steering: ApproachMovingPoint arrives slow at a point on a moving circle")
{
    constexpr f32 maxRelativeSpeed = 60.0f;
    constexpr f32 deceleration = 2.0f;
    constexpr f32 stopRadius = 0.5f;
    constexpr f32 dt = 1.0f / 120.0f;

    f32 worstRelativeSpeed = 0.0f;
    bool allCaptured = true;
    for (const f32 radius : {10.0f, 50.0f})
    {
        for (const f32 omega : {0.2f, 1.0f})
        {
            vec3 bodyPos(0.0f);
            vec3 bodyVel(0.0f);
            bool captured = false;
            for (int step = 0; step < 8000; ++step)
            {
                const f32 t = static_cast<f32>(step) * dt;
                const vec3 targetPos(radius * glm::cos(omega * t), 0.0f,
                                     radius * glm::sin(omega * t));
                const vec3 targetVel(-radius * omega * glm::sin(omega * t), 0.0f,
                                     radius * omega * glm::cos(omega * t));
                bodyVel = ApproachMovingPoint(bodyPos, bodyVel, targetPos, targetVel,
                                              maxRelativeSpeed, deceleration, stopRadius);
                bodyPos += bodyVel * dt;
                if (glm::length(bodyPos - targetPos) <= stopRadius)
                {
                    worstRelativeSpeed =
                        glm::max(worstRelativeSpeed, glm::length(bodyVel - targetVel));
                    captured = true;
                    break;
                }
            }
            allCaptured = allCaptured && captured;
        }
    }
    CHECK(allCaptured);
    CHECK(worstRelativeSpeed < 2.0f); // a latch tolerates a few m/s; this is well under
}

TEST_CASE("math_steering: FacingRates drives the forward error monotonically to zero, rate-capped")
{
    const vec3 maxRates(2.0f, 2.0f, 2.0f);
    constexpr f32 gain = 4.0f;
    constexpr f32 dt = 1.0f / 120.0f;
    const vec3 rateCap = maxRates * dt;

    // A spread of targets, each with an up exactly perpendicular to its forward (so the up can
    // fully align), including a straight-behind forward that forces a half-turn, and an off-axis
    // up to exercise roll.
    struct Target
    {
        vec3 Forward;
        vec3 Up;
    };
    const auto perpendicularUp = [](const vec3& forward, const vec3& hint)
    { return glm::normalize(hint - glm::dot(hint, forward) * forward); };
    const vec3 diagonal = glm::normalize(vec3(1.0f, 1.0f, -1.0f));
    const Target targets[] = {
        {.Forward = glm::normalize(vec3(1.0f, 0.0f, -1.0f)), .Up = vec3(0.0f, 1.0f, 0.0f)},
        {.Forward = glm::normalize(vec3(0.0f, 0.0f, 1.0f)), .Up = vec3(0.0f, 1.0f, 0.0f)},
        {.Forward = diagonal, .Up = perpendicularUp(diagonal, vec3(0.0f, 1.0f, 0.0f))},
    };

    f32 worstFinalForward = 0.0f;
    f32 worstFinalUp = 0.0f;
    f32 worstAxisExceed = 0.0f;
    bool monotone = true;
    for (const Target& target : targets)
    {
        quat orientation(1.0f, 0.0f, 0.0f, 0.0f);
        f32 previousAngle = AngleBetween(orientation * vec3(0.0f, 0.0f, -1.0f), target.Forward);
        for (int step = 0; step < 4000; ++step)
        {
            const vec3 rates =
                FacingRates(orientation, target.Forward, target.Up, maxRates, gain, dt);
            worstAxisExceed = glm::max(worstAxisExceed, glm::abs(rates.x) - rateCap.x);
            worstAxisExceed = glm::max(worstAxisExceed, glm::abs(rates.y) - rateCap.y);
            worstAxisExceed = glm::max(worstAxisExceed, glm::abs(rates.z) - rateCap.z);
            orientation = IntegrateFacing(orientation, rates);
            const f32 angle = AngleBetween(orientation * vec3(0.0f, 0.0f, -1.0f), target.Forward);
            monotone = monotone && angle <= previousAngle + 1e-4f;
            previousAngle = angle;
        }
        worstFinalForward = glm::max(
            worstFinalForward, AngleBetween(orientation * vec3(0.0f, 0.0f, -1.0f), target.Forward));
        worstFinalUp =
            glm::max(worstFinalUp, AngleBetween(orientation * vec3(0.0f, 1.0f, 0.0f), target.Up));
    }
    CHECK(monotone);
    CHECK(worstAxisExceed <= 1e-6f);  // no axis command ever exceeds maxRates * delta
    CHECK(worstFinalForward < 0.01f); // forward converges onto the target
    CHECK(worstFinalUp < 0.01f);      // and roll rolls up onto the target up
}

TEST_CASE("math_steering: FacingRates zeroes roll on an unset up and stays finite at the extremes")
{
    const vec3 maxRates(2.0f, 2.0f, 2.0f);
    constexpr f32 gain = 4.0f;
    constexpr f32 dt = 1.0f / 120.0f;
    const quat identity(1.0f, 0.0f, 0.0f, 0.0f);

    // A zero-length desiredUp means "any up": roll is left at zero.
    const vec3 anyUp = FacingRates(identity, glm::normalize(vec3(1.0f, 0.5f, -1.0f)), vec3(0.0f),
                                   maxRates, gain, dt);
    CHECK(anyUp.z == doctest::Approx(0.0f));

    // Already facing: essentially no command, and finite.
    const vec3 aligned =
        FacingRates(identity, vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f), maxRates, gain, dt);
    CHECK(std::isfinite(aligned.x));
    CHECK(std::isfinite(aligned.y));
    CHECK(std::isfinite(aligned.z));
    CHECK(glm::length(aligned) == doctest::Approx(0.0f).epsilon(1e-4));

    // Exactly opposite: a well-defined half turn, no NaN from a vanishing axis.
    const vec3 opposite =
        FacingRates(identity, vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 1.0f, 0.0f), maxRates, gain, dt);
    CHECK(std::isfinite(opposite.x));
    CHECK(std::isfinite(opposite.y));
    CHECK(std::isfinite(opposite.z));

    // A zero-length desiredForward has no facing to aim at: all three axes are zero.
    const vec3 noTarget =
        FacingRates(identity, vec3(0.0f), vec3(0.0f, 1.0f, 0.0f), maxRates, gain, dt);
    CHECK(noTarget == vec3(0.0f));

    // ShortestArc and AngleBetween at their own degeneracies.
    CHECK(AngleBetween(vec3(0.0f), vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(0.0f));
    const quat half = ShortestArc(vec3(1.0f, 0.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f));
    CHECK(glm::length(half) == doctest::Approx(1.0f));
    const vec3 turned = half * vec3(1.0f, 0.0f, 0.0f);
    CHECK(turned.x == doctest::Approx(-1.0f).epsilon(1e-4));
}
