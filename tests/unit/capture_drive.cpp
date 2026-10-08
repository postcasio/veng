// The bounded decisions the per-frame capture drive makes, all device-free:
//
//  - The claim rule (Renderer/CaptureDrive.h): which scenes' capture surfaces a frame drives, and from
//    which viewport. Only a scene some viewport will render — or a rendering viewport's pending
//    destination — is driven, each once, by its first presenter, so a scene no view shows costs
//    nothing and a scene shown twice is not pushed twice.
//  - The gap rule: a capture driven on consecutive frames resumes its refresh, one whose driver
//    skipped a frame restarts it rather than settling on content that may since have moved.
//  - The capture rotation (Renderer/CaptureRotation.h): the arithmetic ViewportCompositor spends the
//    frame's leftover view budget through — the viewport reservation, and the round-robin that makes a
//    capture set larger than the budget refresh in turn instead of starving its tail.
//
// Driving the claimed scenes needs a device and rides the gpu band (tests/gpu/capture_surface.cpp).

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/CaptureDrive.h"
#include "Renderer/CaptureRotation.h"

#include <set>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("The capture pre-pass claims each rendered scene once, for its first presenter")
{
    TypeRegistry types;
    const Unique<Scene> a = Scene::Create(types);
    const Unique<Scene> b = Scene::Create(types);
    vector<CaptureClaim> claims;

    SUBCASE("Two viewports presenting one scene claim it once, for the earlier")
    {
        const CapturePresenter presenters[] = {
            {.WillRender = true, .Presented = a.get()},
            {.WillRender = true, .Presented = a.get()},
        };
        ClaimCaptureScenes(presenters, claims);
        REQUIRE(claims.size() == 1);
        CHECK(claims[0].World == a.get());
        CHECK(claims[0].Presenter == 0);
        CHECK_FALSE(claims[0].Pending);
    }

    SUBCASE("A viewport that will not render claims nothing, so a later one presenting it does")
    {
        const CapturePresenter presenters[] = {
            {.WillRender = false, .Presented = a.get(), .Pending = b.get()},
            {.WillRender = true, .Presented = a.get()},
        };
        ClaimCaptureScenes(presenters, claims);
        REQUIRE(claims.size() == 1);
        CHECK(claims[0].World == a.get());
        CHECK(claims[0].Presenter == 1);
    }

    SUBCASE("No viewport rendering claims no scene at all")
    {
        const CapturePresenter presenters[] = {
            {.WillRender = false, .Presented = a.get()},
            {.WillRender = false, .Presented = b.get(), .Pending = a.get()},
        };
        ClaimCaptureScenes(presenters, claims);
        CHECK(claims.empty());
    }

    SUBCASE("A rendering viewport's pending destination is claimed beside its presented scene")
    {
        const CapturePresenter presenters[] = {
            {.WillRender = true, .Presented = a.get(), .Pending = b.get()},
        };
        ClaimCaptureScenes(presenters, claims);
        REQUIRE(claims.size() == 2);
        CHECK(claims[1].World == b.get());
        CHECK(claims[1].Presenter == 0);
        CHECK(claims[1].Pending);
    }

    SUBCASE("A destination an earlier viewport already presents is not claimed twice")
    {
        const CapturePresenter presenters[] = {
            {.WillRender = true, .Presented = b.get()},
            {.WillRender = true, .Presented = a.get(), .Pending = b.get()},
        };
        ClaimCaptureScenes(presenters, claims);
        REQUIRE(claims.size() == 2);
        CHECK(claims[0].World == b.get());
        CHECK_FALSE(claims[0].Pending);
        CHECK(claims[1].World == a.get());
    }

    SUBCASE("A viewport holding no scene contributes nothing")
    {
        const CapturePresenter presenters[] = {{.WillRender = true}};
        ClaimCaptureScenes(presenters, claims);
        CHECK(claims.empty());
    }
}

TEST_CASE("A capture's drive restarts its refresh only after a skipped frame")
{
    constexpr u64 Serial = 40;
    // Consecutive frames, or a second drive within one, resume the refresh.
    CHECK_FALSE(CaptureDriveSkippedFrame(Serial, Serial + 1));
    CHECK_FALSE(CaptureDriveSkippedFrame(Serial, Serial));
    // A frame the driver did not render the scene on restarts it, however long the gap.
    CHECK(CaptureDriveSkippedFrame(Serial, Serial + 2));
    CHECK(CaptureDriveSkippedFrame(Serial, Serial + 500));
    // A capture never driven owes its whole first refresh already.
    CHECK_FALSE(CaptureDriveSkippedFrame(0, Serial));
}

TEST_CASE("The capture rotation reserves the viewports' slots and starves no capture")
{
    // One slot per registered viewport is held back: a capture that cannot claim holds its last map,
    // where a viewport that cannot shows a stale window.
    CHECK(CaptureDriveHasRoom(32, 1));
    CHECK(CaptureDriveHasRoom(2, 1));
    CHECK_FALSE(CaptureDriveHasRoom(1, 1));
    CHECK_FALSE(CaptureDriveHasRoom(0, 1));
    // Four presented viewports reserve four, whatever is left over is the captures'.
    CHECK(CaptureDriveHasRoom(5, 4));
    CHECK_FALSE(CaptureDriveHasRoom(4, 4));
    // An empty drive-list has no budget question to answer.
    CHECK_FALSE(CaptureDriveHasRoom(0, 0));

    SUBCASE("A frame that affords the whole list drives it in list order and holds the cursor")
    {
        CHECK(CaptureDriveIndex(0, 0, 4) == 0);
        CHECK(CaptureDriveIndex(0, 3, 4) == 3);
        CHECK(NextCaptureCursor(0, 4, 4) == 0);
        // Order is immaterial when nothing was dropped, so a cursor left mid-list stays put.
        CHECK(NextCaptureCursor(2, 4, 4) == 2);
    }

    SUBCASE("A budget-limited frame resumes at the first capture it could not afford")
    {
        // Three of five driven from cursor 0: the next frame starts on index 3.
        CHECK(NextCaptureCursor(0, 3, 5) == 3);
        // And wraps rather than running off the end.
        CHECK(CaptureDriveIndex(3, 0, 5) == 3);
        CHECK(CaptureDriveIndex(3, 2, 5) == 0);
        CHECK(NextCaptureCursor(3, 3, 5) == 1);
    }

    SUBCASE("Every capture in an over-budget list is reached within ceil(count / budget) frames")
    {
        // The property the rotation exists for: with 9 captures and room for 4 a frame, a fixed
        // prefix would leave the last five permanently black. Walk three frames and collect what was
        // driven — every index must appear.
        constexpr usize Count = 9;
        constexpr usize Budget = 4;
        std::set<usize> driven;
        usize cursor = 0;
        for (int frame = 0; frame < 3; ++frame)
        {
            for (usize step = 0; step < Budget; ++step)
            {
                driven.insert(CaptureDriveIndex(cursor, step, Count));
            }
            cursor = NextCaptureCursor(cursor, Budget, Count);
        }
        CHECK(driven.size() == Count);
    }

    SUBCASE("An empty drive-list is inert")
    {
        CHECK(CaptureDriveIndex(0, 0, 0) == 0);
        CHECK(NextCaptureCursor(0, 0, 0) == 0);
    }
}
