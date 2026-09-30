// Veng::Steering: pure, device-free arithmetic turning a goal into rate commands. No Context.
// The properties pinned here are the ones a mover relies on: the arrive braking curve is monotone,
// bounded, and stops a Euler-integrated body inside its stop radius; a matched-frame approach to a
// point on a moving circle arrives slow; the facing law drives the forward error monotonically
// to zero while never exceeding its per-axis rate caps; closest approach is exact for straight
// lines; and avoidance leaves an unconflicted velocity untouched, keeps the combined radius over
// the horizon when a clear direction exists, honours the preferred side, and never does worse than
// the desired velocity when boxed in.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

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

    // The least clearance a constant velocity keeps from the obstacles over [0, horizon], found by
    // dense sampling rather than the closed form under test. Sampling can only overestimate the
    // true minimum, by well under a millimetre at these speeds and step counts.
    f32 SampledClearance(const vec3& position, const vec3& velocity, const f32 radius,
                         const std::vector<SteeringObstacle>& obstacles, const f32 horizon)
    {
        constexpr int Samples = 1000;
        f32 clearance = std::numeric_limits<f32>::infinity();
        for (const SteeringObstacle& obstacle : obstacles)
        {
            for (int i = 0; i <= Samples; ++i)
            {
                const f32 t = horizon * static_cast<f32>(i) / static_cast<f32>(Samples);
                const f32 separation = glm::length((obstacle.Position + obstacle.Velocity * t) -
                                                   (position + velocity * t));
                clearance = glm::min(clearance, separation - (radius + obstacle.Radius));
            }
        }
        return clearance;
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

TEST_CASE("math_steering: ClosestApproach is exact for straight lines")
{
    // Crossing lines: A leaves the origin along +x at 10, B leaves (0, 0, -100) along +z at 10.
    // At t = 5 they are at (50, 0, 0) and (0, 0, -50), 50·√2 apart, and nearer at no other time.
    const vec3 aPosition(0.0f);
    const vec3 aVelocity(10.0f, 0.0f, 0.0f);
    const vec3 bPosition(0.0f, 0.0f, -100.0f);
    const vec3 bVelocity(0.0f, 0.0f, 10.0f);
    const ClosestApproachResult crossing =
        ClosestApproach(bPosition - aPosition, bVelocity - aVelocity);
    CHECK(crossing.Time == doctest::Approx(5.0f));
    CHECK(crossing.Distance == doctest::Approx(50.0f * std::sqrt(2.0f)));

    // Separating points are closest now.
    const ClosestApproachResult separating =
        ClosestApproach(vec3(10.0f, 0.0f, 0.0f), vec3(3.0f, 4.0f, 0.0f));
    CHECK(separating.Time == 0.0f);
    CHECK(separating.Distance == doctest::Approx(10.0f));

    // No relative velocity: "now", at the current separation.
    const ClosestApproachResult still = ClosestApproach(vec3(0.0f, 6.0f, 8.0f), vec3(0.0f));
    CHECK(still.Time == 0.0f);
    CHECK(still.Distance == doctest::Approx(10.0f));
}

TEST_CASE("math_steering: AvoidObstacles leaves an unconflicted velocity bit-exact")
{
    const vec3 position(0.0f);
    const vec3 desired(0.0f, 0.0f, -100.0f);
    const vec3 up(0.0f, 1.0f, 0.0f);
    constexpr f32 radius = 50.0f;
    constexpr f32 horizon = 4.0f;

    CHECK(AvoidObstacles(position, desired, radius, up, {}, horizon) == desired);

    // Stationary, 500 m off the track: it passes clear inside the horizon.
    const std::array<SteeringObstacle, 1> offTrack{
        {{.Position = vec3(500.0f, 0.0f, -200.0f), .Velocity = vec3(0.0f), .Radius = 100.0f}}};
    CHECK(AvoidObstacles(position, desired, radius, up, offTrack, horizon) == desired);

    // Dead ahead at 2 km, so contact is 18.5 s out: beyond the horizon, it does not count.
    const std::array<SteeringObstacle, 1> farAhead{
        {{.Position = vec3(0.0f, 0.0f, -2000.0f), .Velocity = vec3(0.0f), .Radius = 100.0f}}};
    CHECK(AvoidObstacles(position, desired, radius, up, farAhead, horizon) == desired);
}

TEST_CASE("math_steering: AvoidObstacles keeps the combined radius when a clear direction exists")
{
    const vec3 position(0.0f);
    const vec3 desired(0.0f, 0.0f, -100.0f);
    const vec3 up(0.0f, 1.0f, 0.0f);
    constexpr f32 radius = 50.0f;
    const f32 widestCone = glm::radians(90.0f);

    struct Scenario
    {
        std::vector<SteeringObstacle> Obstacles;
        f32 Horizon;
    };
    const Scenario scenarios[] = {
        // Head-on: an obstacle closing from ahead.
        {.Obstacles = {{.Position = vec3(0.0f, 0.0f, -600.0f),
                        .Velocity = vec3(0.0f, 0.0f, 50.0f),
                        .Radius = 60.0f}},
         .Horizon = 6.0f},
        // Crossing: an obstacle from the right that would meet the mover at (0, 0, -300) at 3 s.
        {.Obstacles = {{.Position = vec3(300.0f, 0.0f, -300.0f),
                        .Velocity = vec3(-100.0f, 0.0f, 0.0f),
                        .Radius = 50.0f}},
         .Horizon = 6.0f},
        // Both at once, plus a slower one drifting in from above.
        {.Obstacles = {{.Position = vec3(0.0f, 0.0f, -600.0f),
                        .Velocity = vec3(0.0f, 0.0f, 50.0f),
                        .Radius = 60.0f},
                       {.Position = vec3(300.0f, 0.0f, -300.0f),
                        .Velocity = vec3(-100.0f, 0.0f, 0.0f),
                        .Radius = 50.0f},
                       {.Position = vec3(0.0f, 200.0f, -400.0f),
                        .Velocity = vec3(0.0f, -30.0f, 0.0f),
                        .Radius = 40.0f}},
         .Horizon = 6.0f},
        // Contact inside the horizon, closest approach after it: head-on from 500 m, closing at
        // 100 m/s, combined radius 150 m. Contact is at 3.5 s, the closest approach at 5 s.
        {.Obstacles = {{.Position = vec3(0.0f, 0.0f, -500.0f),
                        .Velocity = vec3(0.0f),
                        .Radius = 100.0f}},
         .Horizon = 4.0f},
    };

    f32 worstClearance = std::numeric_limits<f32>::infinity();
    f32 widestTurn = 0.0f;
    f32 fastest = 0.0f;
    bool everyDesiredConflicted = true;
    bool anyReturnedUnchanged = false;
    for (const Scenario& scenario : scenarios)
    {
        everyDesiredConflicted =
            everyDesiredConflicted && SampledClearance(position, desired, radius,
                                                       scenario.Obstacles, scenario.Horizon) < 0.0f;
        const vec3 result =
            AvoidObstacles(position, desired, radius, up, scenario.Obstacles, scenario.Horizon);
        anyReturnedUnchanged = anyReturnedUnchanged || result == desired;
        worstClearance =
            glm::min(worstClearance, SampledClearance(position, result, radius, scenario.Obstacles,
                                                      scenario.Horizon));
        widestTurn = glm::max(widestTurn, AngleBetween(result, desired));
        fastest = glm::max(fastest, glm::length(result));
    }
    CHECK(everyDesiredConflicted); // each scenario really does need avoiding
    CHECK_FALSE(anyReturnedUnchanged);
    CHECK(worstClearance >= -1e-3f);
    CHECK(widestTurn <= widestCone + 1e-4f);
    CHECK(fastest <= glm::length(desired) + 1e-3f);
}

TEST_CASE("math_steering: AvoidObstacles passes on the preferred side of up")
{
    const vec3 position(0.0f);
    const vec3 desired(0.0f, 0.0f, -100.0f);
    constexpr f32 radius = 50.0f;
    constexpr f32 horizon = 6.0f;
    const std::array<SteeringObstacle, 1> ahead{
        {{.Position = vec3(0.0f, 0.0f, -400.0f), .Velocity = vec3(0.0f), .Radius = 60.0f}}};

    // Right and left are defined by up: the same obstacle, two different ups.
    for (const vec3& up : {vec3(0.0f, 1.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)})
    {
        const vec3 right = glm::cross(glm::normalize(desired), up);
        const vec3 passRight =
            AvoidObstacles(position, desired, radius, up, ahead, horizon, AvoidSide::Right);
        const vec3 passLeft =
            AvoidObstacles(position, desired, radius, up, ahead, horizon, AvoidSide::Left);
        CHECK(glm::dot(passRight, right) > 0.0f);
        CHECK(glm::dot(passLeft, right) < 0.0f);
    }

    // With no preference, it turns away from an obstacle sitting a little left of its track.
    const vec3 up(0.0f, 1.0f, 0.0f);
    const std::array<SteeringObstacle, 1> leftOfTrack{
        {{.Position = vec3(-30.0f, 0.0f, -400.0f), .Velocity = vec3(0.0f), .Radius = 60.0f}}};
    const vec3 either = AvoidObstacles(position, desired, radius, up, leftOfTrack, horizon);
    CHECK(either.x > 0.0f);
}

TEST_CASE("math_steering: AvoidObstacles boxed in stays finite and no worse than the desired")
{
    // An obstacle on each of the 26 lattice directions around the mover, 150 m out: every
    // direction runs into one within the horizon, even at the slowest candidate speed.
    const vec3 position(10.0f, -20.0f, 5.0f);
    const vec3 desired(0.0f, 0.0f, -50.0f);
    const vec3 up(0.0f, 1.0f, 0.0f);
    constexpr f32 radius = 20.0f;
    constexpr f32 horizon = 20.0f;
    std::vector<SteeringObstacle> obstacles;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int z = -1; z <= 1; ++z)
            {
                if (x == 0 && y == 0 && z == 0)
                {
                    continue;
                }
                const vec3 direction = glm::normalize(vec3(x, y, z));
                obstacles.push_back({.Position = position + direction * 150.0f,
                                     .Velocity = vec3(0.0f),
                                     .Radius = 90.0f});
            }
        }
    }

    const vec3 result = AvoidObstacles(position, desired, radius, up, obstacles, horizon);
    CHECK(std::isfinite(result.x));
    CHECK(std::isfinite(result.y));
    CHECK(std::isfinite(result.z));
    const f32 desiredClearance = SampledClearance(position, desired, radius, obstacles, horizon);
    const f32 resultClearance = SampledClearance(position, result, radius, obstacles, horizon);
    CHECK(resultClearance < 0.0f); // really boxed in: nothing is clear
    CHECK(resultClearance >= desiredClearance - 1e-3f);
}
