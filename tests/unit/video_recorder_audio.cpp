// The recorder's sound track against a driven null audio device: the one path where the mix and the
// frame loop share a clock. What is provable here is that a driven pump delivers exactly the frame's
// samples into the file, in order and with contiguous timestamps from zero — so the sound track and
// the picture track are locked to the sample — and that a block the tap's ring dropped is written as
// silence of exactly the lost length rather than shortening the file's audio against its picture.

#include <doctest/doctest.h>

#include <Capture/RecorderCore.h>

#include <Veng/Audio/AudioDevice.h>

#include <support/FakeVideoRecorderBackend.h>

#include <vector>

using namespace Veng;
using namespace Veng::Capture;

namespace
{
    constexpr u32 SampleRate = 48000;
    constexpr u32 Channels = 2;
    constexpr u32 FrameRate = 60;
    constexpr u32 Pumps = 120;

    /// @brief Opens @p core on @p backend with a PCM sound track at the device's shape.
    VoidResult OpenWithAudio(RecorderCore& core, Unique<Test::FakeVideoRecorderBackend> backend)
    {
        return core.Open(RecorderCoreInfo{
            .Backend = std::move(backend),
            .Settings = {.Lockstep = true, .FrameRate = FrameRate, .Audio = AudioTrack::Pcm},
            .Open =
                {
                    .File = "capture.mov",
                    .Extent = uvec2{640, 360},
                    .FrameRate = FrameRate,
                    .RealTime = false,
                    .Audio = AudioTrack::Pcm,
                    .SampleRate = SampleRate,
                    .Channels = Channels,
                },
            .TapBlockFrames = Audio::AudioDevice::TapBlockFrames,
        });
    }
}

TEST_CASE("video recorder audio: a driven pump's own samples reach the file, contiguous from zero")
{
    const Unique<Audio::AudioDevice> device = Audio::AudioDevice::Create(Audio::AudioDeviceInfo{
        .Backend = Audio::AudioBackend::Null, .SampleRate = SampleRate, .Channels = Channels});
    REQUIRE(device->GetSampleRate() == SampleRate);

    const Unique<RecorderCore> core = CreateUnique<RecorderCore>();
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenWithAudio(*core, std::move(owned)).has_value());

    device->SetBlockTap([&core](std::span<const f32> interleaved, const u32 frames)
                        { core->PushAudio(interleaved, frames, 0); });
    device->SetDriven(true);

    for (u32 pump = 0; pump < Pumps; ++pump)
    {
        device->Pump(1.0f / static_cast<f32>(FrameRate));
    }
    device->SetBlockTap(nullptr);

    CHECK(fake->TotalAudioFrames == static_cast<u64>(Pumps) * (SampleRate / FrameRate));
    CHECK(core->GetState().AudioBlocks == Pumps);
    CHECK(core->GetState().AudioOverruns == 0);

    // Accumulated rather than asserted per block: the claim is that the write position never skips
    // and never repeats, which is one property over the whole run.
    bool contiguous = true;
    u64 expected = 0;
    for (usize block = 0; block < fake->AudioPts.size(); ++block)
    {
        contiguous = contiguous && fake->AudioPts[block] == expected;
        expected += fake->AudioFrames[block];
    }
    CHECK(contiguous);
    CHECK(expected == fake->TotalAudioFrames);
}

TEST_CASE("video recorder audio: a dropped block is written as silence of exactly its length")
{
    const Unique<RecorderCore> core = CreateUnique<RecorderCore>();
    auto owned = CreateUnique<Test::FakeVideoRecorderBackend>();
    Test::FakeVideoRecorderBackend* fake = owned.get();
    REQUIRE(OpenWithAudio(*core, std::move(owned)).has_value());

    const std::vector<f32> block(800 * Channels, 0.5f);
    core->PushAudio(block, 800, 0);
    core->PushAudio(block, 800, 1);

    CHECK(core->GetState().AudioOverruns == 1);
    CHECK(core->GetState().AudioBlocks == 2);
    CHECK(fake->TotalAudioFrames == 800 + Audio::AudioDevice::TapBlockFrames + 800);
    CHECK(fake->NonSilentSamples == 2ULL * 800 * Channels);

    bool contiguous = true;
    u64 expected = 0;
    for (usize entry = 0; entry < fake->AudioPts.size(); ++entry)
    {
        contiguous = contiguous && fake->AudioPts[entry] == expected;
        expected += fake->AudioFrames[entry];
    }
    CHECK(contiguous);
}
