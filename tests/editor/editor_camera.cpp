// EditorCamera pure-math cases: the controller pulls in no ImGui/Window/Input, so it
// drives device-free off hand-filled EditorCameraInput frames. Assertions stay tolerant
// (camera math) — directions and monotonic changes, not exact floats.

#include <doctest/doctest.h>

#include "EditorCamera.h"

using namespace VengEditor;
using Veng::f32;
using Veng::vec3;

namespace
{
    // A neutral, no-input frame at a square aspect.
    EditorCameraInput IdleFrame()
    {
        EditorCameraInput in;
        in.Aspect = 1.0f;
        return in;
    }

    // The camera's world-space eye, read off the rebuilt view.
    vec3 EyeOf(const EditorCamera& camera)
    {
        return camera.GetView().GetPosition();
    }

    // The camera's world-space forward (toward what it looks at): the negated +Z
    // column of the view's inverse (the camera's world matrix).
    vec3 ForwardOf(const EditorCamera& camera)
    {
        const Veng::mat4 world = glm::inverse(camera.GetView().View());
        return -glm::normalize(vec3(world[2]));
    }
}

TEST_CASE("Alt+LMB orbit moves the eye but keeps it looking near the pivot")
{
    EditorCamera camera;
    camera.Update(IdleFrame(), 0.016f);
    const vec3 eyeBefore = EyeOf(camera);

    EditorCameraInput orbit = IdleFrame();
    orbit.Focused = true;
    orbit.Alt = true;
    orbit.MouseLeft = true;
    orbit.MouseDelta = {120.0f, 40.0f};
    camera.Update(orbit, 0.016f);

    const vec3 eyeAfter = EyeOf(camera);

    // Orbiting revolves the eye around the pivot, so it moves.
    CHECK(glm::length(eyeAfter - eyeBefore) > 0.01f);

    // Orbit preserves the pivot distance: before and after, the eye sits the same
    // radius from the pivot (the default pivot is the origin).
    const vec3 pivot{0.0f};
    CHECK(glm::length(eyeAfter - pivot) ==
          doctest::Approx(glm::length(eyeBefore - pivot)).epsilon(0.02f));

    // The view still faces the pivot: the forward axis points from eye toward origin.
    const vec3 forward = ForwardOf(camera);
    const vec3 toPivot = glm::normalize(pivot - eyeAfter);
    CHECK(glm::dot(forward, toPivot) > 0.9f);
}

TEST_CASE("Frame recenters the pivot so the view looks at the given center")
{
    EditorCamera camera;

    const vec3 center{5.0f, 1.0f, -2.0f};
    camera.Frame(center, 1.0f);
    // Frame leaves the view stale until the next Update rebuilds it against the aspect.
    camera.Update(IdleFrame(), 0.016f);

    const vec3 eye = EyeOf(camera);
    const vec3 forward = ForwardOf(camera);
    const vec3 toCenter = glm::normalize(center - eye);

    // The framed eye looks straight at the center.
    CHECK(glm::dot(forward, toCenter) > 0.99f);
    // And it is pulled back from the center, not sitting on it.
    CHECK(glm::length(center - eye) > 0.5f);
}

TEST_CASE("Scroll dollies the eye toward the pivot")
{
    EditorCamera camera;
    camera.Frame(vec3(0.0f), 1.0f);
    camera.Update(IdleFrame(), 0.016f);
    const f32 distanceBefore = glm::length(EyeOf(camera) - vec3(0.0f));

    EditorCameraInput zoomIn = IdleFrame();
    zoomIn.Hovered = true;
    zoomIn.ScrollDelta = {0.0f, 1.0f};
    camera.Update(zoomIn, 0.016f);
    const f32 distanceAfter = glm::length(EyeOf(camera) - vec3(0.0f));

    // A positive scroll dolly-zooms in: the eye gets closer to the pivot.
    CHECK(distanceAfter < distanceBefore);

    // Scrolling back out moves it away again.
    EditorCameraInput zoomOut = IdleFrame();
    zoomOut.Hovered = true;
    zoomOut.ScrollDelta = {0.0f, -1.0f};
    camera.Update(zoomOut, 0.016f);
    const f32 distanceOut = glm::length(EyeOf(camera) - vec3(0.0f));
    CHECK(distanceOut > distanceAfter);
}

TEST_CASE("SetFlySpeed and SetFovY round-trip through the getters")
{
    EditorCamera camera;
    camera.SetFlySpeed(12.0f);
    CHECK(camera.GetFlySpeed() == doctest::Approx(12.0f));

    camera.SetFovY(glm::radians(60.0f));
    CHECK(camera.GetFovY() == doctest::Approx(glm::radians(60.0f)));
}
