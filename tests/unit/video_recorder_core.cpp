// The video recorder's device-free half, driven against a recording fake encoder and an injected
// clock. What is provable here is the whole of the recorder's contract that does not need a GPU:
// a frame is composited into a buffer and appended only once its slot's fence has been waited, so
// the appended count trails the acquired one by the frames in flight and never more; every buffer
// the pool vends is released exactly once, after its append, which is what lets the pool recycle a
// handful of surfaces across a whole capture; the two timestamp modes; that Stop needs no future
// frame; and that the four ways a capture ends early each end it with a reason.

#include <doctest/doctest.h>

#include <Capture/RecorderCore.h>

#include <Veng/Diagnostics/Profiler.h>

#include <support/FakeVideoRecorderBackend.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>

using namespace Veng;
using namespace Veng::Capture;

namespace
{
    constexpr u32 FramesInFlight = 3;
    constexpr uvec2 TestExtent{1920, 1080};

    /// @brief A clock the case moves by hand, shared with the hooks that read it.
    using Clock = Ref<f64>;

    /// @brief Builds a core reading @p clock, with the wait step doing nothing.
    Unique<RecorderCore> MakeCore(const Clock& clock)
    {
        return CreateUnique<RecorderCore>(RecorderCore::Hooks{
            .Now = [clock] { return *clock; },
            .WaitStep = [] {},
        });
    }

    /// @brief Opens @p core on @p backend with @p settings, and reports whether it took.
    VoidResult OpenCore(RecorderCore& core, Unique<Test::FakeVideoRecorderBackend> backend,
                        const VideoCaptureSettings& settings)
    {
        return core.Open(RecorderCoreInfo{
            .Backend = std::move(backend),
            .Settings = settings,
            .Open =
                {
                    .File = "capture.mov",
                    .Extent = TestExtent,
                    .Encoding = settings.Encoding,
                    .Codec = settings.Codec,
                    .BitsPerSecond = DeriveBitsPerSecond(settings, TestExtent),
                    .FrameRate = settings.FrameRate,
                    .RealTime = !settings.Lockstep,
                    .Audio = settings.Audio,
                    .Channels = 2,
                    .PoolAllocationThreshold = FramesInFlight + 4,
                },
            .TapBlockFrames = 1024,
        });
    }

    /// @brief The lockstep settings the timing cases share.
    VideoCaptureSettings LockstepSettings()
    {
        return VideoCaptureSettings{
            .Encoding = CaptureEncoding::Sdr,
            .Lockstep = true,
            .FrameRate = 60,
            .Audio = AudioTrack::None,
        };
    }
}

TEST_CASE("video recorder core: appends trail the frames in flight and release after their append")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    constexpr u64 Frames = 12;
    u64 worstTrail = 0;
    for (u64 frame = 0; frame < Frames; ++frame)
    {
        const u32 slot = static_cast<u32>(frame % FramesInFlight);
        // The engine's order: the slot retires at the top of the frame, then the composite asks for
        // this frame's target.
        core->Retire(slot);
        REQUIRE(core->Acquire(slot).has_value());

        const VideoCaptureState state = core->GetState();
        worstTrail = std::max(worstTrail, state.FramesAcquired - state.FramesAppended);
    }

    CHECK(worstTrail == FramesInFlight);
    CHECK(core->GetState().FramesAppended == Frames - FramesInFlight);

    core->Finish();

    const VideoCaptureState state = core->GetState();
    CHECK(state.FramesAcquired == Frames);
    CHECK(state.FramesAppended == Frames);
    CHECK(fake->ReleasedAfterAppend);
    CHECK(fake->Outstanding == 0);
    CHECK(fake->Releases.size() == Frames);
    CHECK(std::ranges::all_of(fake->Releases, [](const auto& entry) { return entry.second == 1; }));
    CHECK(fake->Finished);
}

TEST_CASE("video recorder core: lockstep stamps k over the frame rate from its own acquire count")
{
    const Clock clock = std::make_shared<f64>(1000.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    for (u64 frame = 0; frame < 6; ++frame)
    {
        const u32 slot = static_cast<u32>(frame % FramesInFlight);
        core->Retire(slot);
        REQUIRE(core->Acquire(slot).has_value());
        // A lockstep frame takes however long it takes; the stamps must not notice.
        *clock += 0.25;
    }
    core->Finish();

    REQUIRE(fake->VideoPts.size() == 6);
    bool exact = true;
    bool increasing = true;
    for (usize frame = 0; frame < fake->VideoPts.size(); ++frame)
    {
        exact = exact && fake->VideoPts[frame] == static_cast<i64>(frame) * (VideoTimescale / 60);
        increasing =
            increasing && (frame == 0 || fake->VideoPts[frame] > fake->VideoPts[frame - 1]);
    }
    CHECK(exact);
    CHECK(increasing);
    CHECK(core->GetState().DurationSeconds == doctest::Approx(5.0 / 60.0));
}

TEST_CASE("video recorder core: real time stamps the wall clock rebased, never repeating")
{
    const Clock clock = std::make_shared<f64>(500.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();

    VideoCaptureSettings settings = LockstepSettings();
    settings.Lockstep = false;
    REQUIRE(OpenCore(*core, std::move(owned), settings).has_value());

    const f64 offsets[] = {0.0, 0.02, 0.02, 0.5};
    for (usize frame = 0; frame < std::size(offsets); ++frame)
    {
        *clock = 500.0 + offsets[frame];
        core->Retire(static_cast<u32>(frame % FramesInFlight));
        REQUIRE(core->Acquire(static_cast<u32>(frame % FramesInFlight)).has_value());
    }
    core->Finish();

    REQUIRE(fake->VideoPts.size() == 4);
    CHECK(fake->VideoPts[0] == 0);
    CHECK(fake->VideoPts[1] == 1200);
    // The same wall time twice: the writer requires strictly increasing stamps, so the second frame
    // is pushed on by exactly one tick rather than being refused.
    CHECK(fake->VideoPts[2] == 1201);
    CHECK(fake->VideoPts[3] == 30000);
}

TEST_CASE("video recorder core: Stop drains the outstanding slots in order with no further frame")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    // Three frames composited, none retired — the window closed, or the app is quitting.
    for (u32 slot = 0; slot < FramesInFlight; ++slot)
    {
        REQUIRE(core->Acquire(slot).has_value());
    }
    CHECK(core->GetState().FramesAppended == 0);

    core->Finish();

    CHECK(core->GetState().FramesAppended == FramesInFlight);
    CHECK(fake->VideoPts == vector<i64>{0, VideoTimescale / 60, 2 * (VideoTimescale / 60)});
    CHECK(fake->Outstanding == 0);
    CHECK(fake->Finished);
    CHECK(core->IsFinalizing());

    core->WaitForFinalize();
    CHECK(core->GetState().Status == VideoCaptureStatus::Off);
}

TEST_CASE("video recorder core: a frame budget stops the capture at its last frame")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();

    VideoCaptureSettings settings = LockstepSettings();
    settings.FrameBudget = 10;
    REQUIRE(OpenCore(*core, std::move(owned), settings).has_value());

    for (u64 frame = 0; frame < 10; ++frame)
    {
        const u32 slot = static_cast<u32>(frame % FramesInFlight);
        core->Retire(slot);
        REQUIRE(core->Acquire(slot).has_value());
    }

    CHECK(core->WantsStop());
    CHECK_FALSE(core->Acquire(0).has_value());

    core->Finish();
    CHECK(core->GetState().FramesAcquired == 10);
    CHECK(core->GetState().FramesAppended == 10);
    CHECK(fake->VideoPts.size() == 10);
}

TEST_CASE("video recorder core: an encoder that never becomes ready ends the capture at the bound")
{
    // A clock that advances half a second per reading, so the two-second bound is reached in four.
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = CreateUnique<RecorderCore>(RecorderCore::Hooks{
        .Now =
            [clock]
        {
            const f64 reading = *clock;
            *clock += 0.5;
            return reading;
        },
        .WaitStep = [] {},
    });

    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    REQUIRE(core->Acquire(0).has_value());
    fake->Ready = false;

    CHECK_FALSE(core->Acquire(1).has_value());

    const VideoCaptureState state = core->GetState();
    CHECK(state.LastError == "encoder stalled");
    CHECK(state.FramesWaited == 1);
    CHECK(state.WaitedForEncoderMs >= RecorderCore::EncoderWaitBoundSeconds * 1000.0);
    // The frame already in flight is still drained into the file rather than being lost with it.
    CHECK(state.FramesAppended == 1);
    CHECK(fake->Finished);
    CHECK(fake->Outstanding == 0);
}

TEST_CASE("video recorder core: a failed writer ends the capture in its own words")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    REQUIRE(core->Acquire(0).has_value());
    fake->FailWith = "The disk is full.";

    CHECK_FALSE(core->Acquire(1).has_value());
    CHECK(core->GetState().LastError == "The disk is full.");
    CHECK(core->GetState().FramesAppended == 1);
    CHECK(fake->Finished);
}

TEST_CASE("video recorder core: an aborted capture keeps the file intact to its last frame")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenCore(*core, std::move(owned), LockstepSettings()).has_value());

    for (u32 slot = 0; slot < FramesInFlight; ++slot)
    {
        REQUIRE(core->Acquire(slot).has_value());
    }

    core->Abort("swap chain changed");

    CHECK(core->GetState().LastError == "swap chain changed");
    CHECK(core->GetState().FramesAppended == FramesInFlight);
    CHECK(fake->Outstanding == 0);

    core->WaitForFinalize();
    CHECK(core->GetState().Status == VideoCaptureStatus::Off);
}

TEST_CASE("video recorder core: a second capture is refused while one is recording")
{
    const Clock clock = std::make_shared<f64>(0.0);
    const Unique<RecorderCore> core = MakeCore(clock);
    REQUIRE(OpenCore(*core, CreateUnique<Test::FakeVideoRecorderBackend>(), LockstepSettings())
                .has_value());

    const VoidResult second =
        OpenCore(*core, CreateUnique<Test::FakeVideoRecorderBackend>(), LockstepSettings());
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == "A capture is already recording.");
}

TEST_CASE("video recorder core: settings the encoder cannot honour are refused with a reason")
{
    VideoCaptureSettings settings{.Encoding = CaptureEncoding::Hdr10, .Codec = VideoCodec::H264};
    CHECK_FALSE(CodecSupportsEncoding(VideoCodec::H264, CaptureEncoding::Hdr10));
    CHECK(CodecSupportsEncoding(VideoCodec::Hevc, CaptureEncoding::Hdr10));
    CHECK(CodecSupportsEncoding(VideoCodec::ProRes4444, CaptureEncoding::Hdr10));
    CHECK_FALSE(ValidateCaptureSettings(settings, 2).has_value());

    settings = VideoCaptureSettings{.FrameRate = 0};
    CHECK_FALSE(ValidateCaptureSettings(settings, 2).has_value());

    settings = VideoCaptureSettings{.Audio = AudioTrack::Pcm};
    CHECK_FALSE(ValidateCaptureSettings(settings, 3).has_value());
    CHECK(ValidateCaptureSettings(settings, 1).has_value());
    CHECK(ValidateCaptureSettings(settings, 2).has_value());

    // A device the writer has no layout for still records a picture, just no sound.
    settings.Audio = AudioTrack::None;
    CHECK(ValidateCaptureSettings(settings, 6).has_value());
}

TEST_CASE("video recorder core: the bitrate follows the picture, and an override wins")
{
    // A rounding band rather than an equality: the quality is a float, so the product lands within
    // a bit or two of the exact figure. 1080p60 at 0.15 bpp is 18.7 Mbps.
    const auto bits = [](const VideoCaptureSettings& settings, const uvec2 extent)
    { return static_cast<i64>(DeriveBitsPerSecond(settings, extent)); };

    const VideoCaptureSettings derived{.BitsPerPixel = 0.15f, .FrameRate = 60};
    CHECK(std::abs(bits(derived, uvec2{1920, 1080}) - 18662400) <= 8);
    // A quarter of the pixels: one quality figure serves a small window too.
    CHECK(std::abs(bits(derived, uvec2{960, 540}) - 4665600) <= 8);

    VideoCaptureSettings overridden = derived;
    overridden.BitrateMbps = 50;
    CHECK(bits(overridden, uvec2{1920, 1080}) == 50000000);

    CHECK(DescribeCodec(VideoCodec::Hevc, CaptureEncoding::Hdr10) == "HEVC Main 10");
    CHECK(DescribeCodec(VideoCodec::Hevc, CaptureEncoding::Sdr) == "HEVC Main");
}

TEST_CASE("video recorder core: an unnamed capture is named for the app and the moment")
{
    const string name = DefaultCaptureName("Test App", 1'700'000'000);

    // The stamp's shape rather than its value: what it reads is the host's time zone.
    REQUIRE(name.size() == string("Test-App").size() + 1 + 8 + 1 + 6);
    CHECK(name.starts_with("Test-App-"));
    CHECK(name[string("Test-App-").size() + 8] == '-');
    CHECK(std::ranges::all_of(name.substr(string("Test-App-").size(), 8),
                              [](const char c) { return c >= '0' && c <= '9'; }));
    CHECK(std::ranges::all_of(name.substr(string("Test-App-").size() + 9),
                              [](const char c) { return c >= '0' && c <= '9'; }));

    // A name is a name, never a path: only its final component reaches the capture directory.
    const path resolved = ResolveVideoCapturePath("../../elsewhere/run");
    CHECK(resolved.filename() == path("run.mov"));
    CHECK(resolved.parent_path() == Diagnostics::CaptureDirectory());
}
