// The recorder on a run that cannot record: every context in this band is headless, and a headless
// run presents no frame. Two claims worth pinning because both are failure paths nothing else
// exercises: constructing the recorder against a swap-chain-less context does not touch a swap chain
// (the invalidation subscription is guarded), and a refused Start says why rather than failing mute.

#include <doctest/doctest.h>

#include <Capture/VideoRecorderBackend.h>

#include <Veng/Audio/AudioDevice.h>
#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Renderer/ViewportCompositor.h>

#include <gpu/fixture.h>

using namespace Veng;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "video recorder: a headless run reports itself unavailable")
{
    Renderer::ViewportCompositor compositor(Context);
    const Unique<Audio::AudioDevice> device = Audio::AudioDevice::Create(
        Audio::AudioDeviceInfo{.Backend = Audio::AudioBackend::Null, .Channels = 2});

    Capture::VideoRecorder recorder(Context, compositor, *device,
                                    Capture::VideoRecorderHost{.Name = "veng-test"});

    REQUIRE(Context.IsHeadless());
    CHECK_FALSE(recorder.IsAvailable());
    CHECK_FALSE(recorder.IsRecording());

    CHECK_FALSE(recorder.Start(Capture::VideoCaptureSettings{}));

    const Capture::VideoCaptureState state = recorder.GetState();
    CHECK(state.Status == Capture::VideoCaptureStatus::Off);
    CHECK_FALSE(state.LastError.empty());
    CHECK(state.FramesAcquired == 0);
}

TEST_CASE("video recorder: the backend exists only where the platform has an encoder")
{
#if defined(__APPLE__)
    CHECK(Capture::IsVideoRecorderBackendCompiled());
    CHECK(Capture::CreateVideoRecorderBackend() != nullptr);
#else
    CHECK_FALSE(Capture::IsVideoRecorderBackendCompiled());
    CHECK(Capture::CreateVideoRecorderBackend() == nullptr);
#endif
}
