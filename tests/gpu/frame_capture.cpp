// The still-capture seams that a headless context can reach: the compositor serving several capture
// sinks through the frame-retirement dispatch, the frame capture refusing where no frame is
// composited, and a real image read back into a picture. The capture composite itself runs only
// windowed, so it is not reached here.

#include <doctest/doctest.h>

#include <Veng/Capture/FrameCapture.h>
#include <Veng/Capture/Screenshot.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ViewportCompositor.h>

#include <gpu/fixture.h>

#include <array>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    /// @brief A sink that wants no target and counts the retirements it hears.
    class CountingSink final : public CaptureSink
    {
    public:
        optional<CaptureTarget> AcquireTarget(u32 /*slot*/, uvec2 /*presentedExtent*/) override
        {
            return std::nullopt;
        }

        void OnSlotRetired(u32 /*slot*/) override
        {
            ++Retirements;
            if (OnRetired)
            {
                OnRetired();
            }
        }

        u32 Retirements = 0;
        function<void()> OnRetired;
    };
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "compositor: every installed sink hears each retirement, and a removed one none")
{
    ViewportCompositor compositor(Context);
    CountingSink first;
    CountingSink second;

    compositor.AddCaptureSink(first);
    compositor.AddCaptureSink(second);
    compositor.AddCaptureSink(first);
    CHECK(compositor.HasCaptureSink(first));
    CHECK(compositor.HasCaptureSink(second));

    const u32 frames = Context.GetMaxFramesInFlight() * 2;
    for (u32 frame = 0; frame < frames; ++frame)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }
    // Installed twice is installed once.
    CHECK(first.Retirements == frames);
    CHECK(second.Retirements == frames);

    // A sink removing another during the dispatch — and itself — silences both from that moment.
    first.OnRetired = [&compositor, &first, &second]
    {
        compositor.RemoveCaptureSink(second);
        compositor.RemoveCaptureSink(first);
    };
    Context.BeginFrame();
    Context.EndFrame();
    CHECK(first.Retirements == frames + 1);
    CHECK(second.Retirements == frames);
    CHECK_FALSE(compositor.HasCaptureSink(first));
    CHECK_FALSE(compositor.HasCaptureSink(second));

    Context.BeginFrame();
    Context.EndFrame();
    CHECK(first.Retirements == frames + 1);

    Context.WaitIdle();
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "frame capture: a headless run reports itself unavailable and refuses with a reason")
{
    ViewportCompositor compositor(Context);
    Capture::FrameCapture capture(Context, compositor);

    REQUIRE(Context.IsHeadless());
    CHECK_FALSE(capture.IsAvailable());

    const VoidResult requested = capture.Request({});
    REQUIRE_FALSE(requested.has_value());
    CHECK_FALSE(requested.error().empty());
    CHECK_FALSE(capture.IsOpen());
    CHECK_FALSE(capture.IsPending());
    CHECK_FALSE(compositor.HasCaptureSink(capture));

    const Result<Capture::RgbImage> taken = capture.Take();
    REQUIRE_FALSE(taken.has_value());
    CHECK_FALSE(taken.error().empty());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "screenshot: an image reads back as the picture it holds")
{
    const Ref<Image> image =
        Image::Create(Context, {
                                   .Name = "Screenshot Readback",
                                   .Extent = {2, 2, 1},
                                   .Format = Format::BGRA8Srgb,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferSrc |
                                            ImageUsage::TransferDst,
                               });
    // BGRA texels: blue, green, red, white. Sampled because UploadSync leaves the image shader-read.
    const std::array<u8, 16> texels{255, 0, 0,   255, 0,   255, 0,   255,
                                    0,   0, 255, 255, 255, 255, 255, 255};
    image->UploadSync(texels);

    const Result<Capture::RgbImage> picture = Capture::ReadImageRgb8(*image);
    REQUIRE(picture.has_value());
    CHECK(picture->Width == 2);
    CHECK(picture->Height == 2);
    CHECK(picture->Pixels == vector<u8>{0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255});
}
