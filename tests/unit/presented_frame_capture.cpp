// The presented-frame mirror's request latch: which frame ends pay the full-window blit. The
// latch is what EndFrame consults, so a frame end with no capture requested records nothing and a
// requested one records exactly one frame — proven here without a swap chain, which no test
// process has.

#include <doctest/doctest.h>

#include <Veng/Renderer/Backend/PresentedFrameCapture.h>

using namespace Veng::Renderer::Backend;

TEST_CASE("presented-frame capture: no request mirrors no frame")
{
    PresentedFrameCaptureLatch latch;
    CHECK_FALSE(latch.IsPending());

    int mirrored = 0;
    for (int frame = 0; frame < 8; ++frame)
    {
        mirrored += latch.TakeForFrame() ? 1 : 0;
    }
    CHECK(mirrored == 0);
}

TEST_CASE("presented-frame capture: a request mirrors the next frame end and only it")
{
    PresentedFrameCaptureLatch latch;
    latch.Request();
    CHECK(latch.IsPending());

    CHECK(latch.TakeForFrame());
    CHECK_FALSE(latch.IsPending());
    CHECK_FALSE(latch.TakeForFrame());
}

TEST_CASE("presented-frame capture: requests before a frame end coalesce into it")
{
    PresentedFrameCaptureLatch latch;
    latch.Request();
    latch.Request();
    latch.Request();

    CHECK(latch.TakeForFrame());
    CHECK_FALSE(latch.TakeForFrame());

    // A request after that frame end is a fresh one, serviced by the frame end after it.
    latch.Request();
    CHECK(latch.TakeForFrame());
}
