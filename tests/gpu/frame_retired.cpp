// The frame-retired notification: a registered callback fires once per frame-in-flight slot, for
// the slot whose fence BeginFrame just waited, and stops when the handle is released. The contract
// a consumer reading what a frame wrote depends on — its work is complete when its slot retires.
//
// Also pins the headless answer of AddSwapChainInvalidationCallback: no swap chain means nothing to
// invalidate, so registering is inert rather than a null dereference.

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "frame retirement announces each slot once per cycle")
{
    const u32 slots = Context.GetMaxFramesInFlight();
    REQUIRE(slots >= 1);

    vector<u32> announced;
    const Context::FrameRetiredHandle handle = Context.AddFrameRetiredCallback(
        [&announced](const u32 slot) { announced.push_back(slot); });
    CHECK(handle != 0);

    // Three full cycles: enough that a slot announced more or less than once per cycle, or out of
    // order, shows up as a wrong sequence rather than a coincidence.
    const u32 frames = slots * 3;
    for (u32 frame = 0; frame < frames; ++frame)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }

    REQUIRE(announced.size() == frames);

    // The slot BeginFrame announces is the one it is about to record into, so the sequence is the
    // round-robin the frames themselves walk.
    bool inOrder = true;
    for (u32 frame = 0; frame < frames; ++frame)
    {
        inOrder = inOrder && announced[frame] == frame % slots;
    }
    CHECK(inOrder);

    Context.RemoveFrameRetiredCallback(handle);

    const usize before = announced.size();
    for (u32 frame = 0; frame < slots; ++frame)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }
    CHECK(announced.size() == before);

    // Releasing a handle that names no live registration is silent, so a double release is safe.
    Context.RemoveFrameRetiredCallback(handle);

    Context.WaitIdle();
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "a headless context accepts a swap chain invalidation")
{
    REQUIRE(Context.IsHeadless());
    CHECK_FALSE(Context.IsSwapChainCaptureSupported());

    bool fired = false;
    Context.AddSwapChainInvalidationCallback([&fired] { fired = true; });

    Context.BeginFrame();
    Context.EndFrame();
    Context.WaitIdle();

    CHECK_FALSE(fired);
}
