// Hi-Z history-validity transitions: pure CPU, no Context, no Vulkan. The
// occlusion test trusts last frame's pyramid only when the view is steady; this
// pins both sides of every invalidation boundary — the property that makes
// "occlusion never drops a visible draw" hold across discontinuities (a missed
// invalidation could false-cull; an extra invalidation only draws more).
//
// Frame-0 and post-resize invalidation are the renderer's own gate (the reset
// flag), tested through SceneRenderer's GPU path; here the device-free metric is
// pinned directly.

#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Renderer/HiZHistory.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A scene bound diagonal the translation threshold is a fraction of.
    constexpr f32 SceneDiagonal = 100.0f;

    HiZHistoryState SteadyState()
    {
        return HiZHistoryState{
            .CameraPosition = vec3(0.0f, 0.0f, 10.0f),
            .CameraForward = vec3(0.0f, 0.0f, -1.0f),
            .Projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f),
        };
    }
}

TEST_CASE("Hi-Z history is valid for a steady camera and extent")
{
    const HiZHistoryState state = SteadyState();
    CHECK(IsHiZHistoryValid(state, state, SceneDiagonal, HiZHistorySettings{}));
}

TEST_CASE("Hi-Z history invalidates on any projection change")
{
    const HiZHistoryState previous = SteadyState();
    HiZHistoryState current = previous;
    // A different FOV — the projection differs, which misaligns the footprint.
    current.Projection = glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    CHECK_FALSE(IsHiZHistoryValid(previous, current, SceneDiagonal, HiZHistorySettings{}));
}

TEST_CASE("Hi-Z history brackets the translation threshold")
{
    const HiZHistorySettings settings{};
    const HiZHistoryState previous = SteadyState();
    const f32 limit = settings.TranslationFraction * SceneDiagonal;

    SUBCASE("just under the threshold is valid")
    {
        HiZHistoryState current = previous;
        current.CameraPosition.x += limit * 0.99f;
        CHECK(IsHiZHistoryValid(previous, current, SceneDiagonal, settings));
    }
    SUBCASE("just over the threshold is invalid")
    {
        HiZHistoryState current = previous;
        current.CameraPosition.x += limit * 1.01f;
        CHECK_FALSE(IsHiZHistoryValid(previous, current, SceneDiagonal, settings));
    }
}

TEST_CASE("Hi-Z history brackets the rotation threshold")
{
    const HiZHistorySettings settings{};
    const HiZHistoryState previous = SteadyState();

    const auto rotatedForward = [](f32 angle)
    {
        // Rotate the -Z forward about Y by `angle`.
        return vec3(std::sin(angle) * -1.0f, 0.0f, std::cos(angle) * -1.0f);
    };

    SUBCASE("just under the threshold is valid")
    {
        HiZHistoryState current = previous;
        current.CameraForward = rotatedForward(settings.RotationRadians * 0.99f);
        CHECK(IsHiZHistoryValid(previous, current, SceneDiagonal, settings));
    }
    SUBCASE("just over the threshold is invalid")
    {
        HiZHistoryState current = previous;
        current.CameraForward = rotatedForward(settings.RotationRadians * 1.01f);
        CHECK_FALSE(IsHiZHistoryValid(previous, current, SceneDiagonal, settings));
    }
}

TEST_CASE("Hi-Z history invalidates any nonzero move in an unbounded (zero-diagonal) scene")
{
    // A zero scene diagonal makes the translation limit zero, so any move at all
    // invalidates — the conservative direction when there is no bound to scale by.
    const HiZHistoryState previous = SteadyState();
    HiZHistoryState current = previous;
    current.CameraPosition.x += 0.001f;
    CHECK_FALSE(IsHiZHistoryValid(previous, current, 0.0f, HiZHistorySettings{}));
}
