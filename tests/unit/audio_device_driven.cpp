// The device's driven mode and its block tap, on the null backend — the band has no audio hardware,
// and the null device already mixes on the main thread, which is exactly the path driven mode puts a
// hardware device on. What is provable here: a pump delivers exactly delta * sampleRate frames to
// the tap, the hitch clamp that bounds a measured delta is lifted for a driven one, a cleared tap
// receives nothing further, and the callback-side ring drops the newest block when full and counts
// it. Stopping and restarting real hardware needs a device and is not reachable from the band.

#include <doctest/doctest.h>

#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>

#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    constexpr u32 TestSampleRate = 48000;

    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(AudioDeviceInfo{
            .Backend = AudioBackend::Null, .SampleRate = TestSampleRate, .Channels = 2});
    }

    // Accumulates every tapped block: the total frames, the number of calls, whether every block's
    // span matched its frame count, and each block's first sample, which is what ordering is read
    // off. Accumulating is what keeps a hundred-block run to a handful of assertions.
    struct TapRecord
    {
        u64 Frames = 0;
        u64 Calls = 0;
        bool SpansMatch = true;
        std::vector<f32> FirstSamples;

        void Take(std::span<const f32> interleaved, u32 frames)
        {
            Frames += frames;
            ++Calls;
            SpansMatch = SpansMatch && interleaved.size() == static_cast<usize>(frames) * 2;
            FirstSamples.push_back(interleaved.empty() ? 0.0f : interleaved[0]);
        }
    };
}

TEST_CASE("audio driven: SetDriven toggles the mode and leaves a null device otherwise unchanged")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    CHECK_FALSE(device->IsDriven());

    device->SetDriven(true);
    CHECK(device->IsDriven());
    CHECK(device->IsNull());
    CHECK(device->GetSampleRate() == TestSampleRate);
    CHECK(device->GetChannels() == 2);

    device->SetDriven(false);
    CHECK_FALSE(device->IsDriven());
}

TEST_CASE("audio driven: a pump delivers exactly its frame's samples to the tap, in order")
{
    const Unique<AudioDevice> device = MakeNullDevice();

    TapRecord record;
    device->SetBlockTap([&record](std::span<const f32> interleaved, u32 frames)
                        { record.Take(interleaved, frames); });

    device->Pump(1.0f / 60.0f);
    CHECK(record.Calls == 1);
    CHECK(record.Frames == 800);

    for (u32 i = 0; i < 119; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK(record.Calls == 120);
    CHECK(record.Frames == 96000);
    CHECK(record.SpansMatch);

    // Clearing the tap stops delivery; nothing arrives after.
    device->SetBlockTap(nullptr);
    device->Pump(1.0f / 60.0f);
    CHECK(record.Calls == 120);
    CHECK(record.Frames == 96000);
}

TEST_CASE("audio driven: the hitch clamp bounds a measured pump but not a driven one")
{
    const Unique<AudioDevice> device = MakeNullDevice();

    TapRecord wall;
    device->SetBlockTap([&wall](std::span<const f32> interleaved, u32 frames)
                        { wall.Take(interleaved, frames); });

    // Wall mode: half a second of virtual time is clamped to the 100 ms hitch bound, so the mixer
    // never grinds through a stalled frame's worth of audio.
    device->Pump(0.5f);
    CHECK(wall.Frames == 4800);

    TapRecord driven;
    device->SetBlockTap([&driven](std::span<const f32> interleaved, u32 frames)
                        { driven.Take(interleaved, frames); });

    // Driven: the delta is exact, so the full half second is mixed.
    device->SetDriven(true);
    device->Pump(0.5f);
    CHECK(driven.Frames == 24000);
}

TEST_CASE("audio driven: a full tap ring drops the newest block and counts the loss")
{
    const Unique<AudioDevice> device = MakeNullDevice();

    TapRecord record;
    device->SetBlockTap([&record](std::span<const f32> interleaved, u32 frames)
                        { record.Take(interleaved, frames); });
    CHECK(device->GetTapOverruns() == 0);

    // The callback's side: 70 blocks into a 64-slot ring, each stamped with its index so the
    // delivered order and identity are readable.
    constexpr u32 blocks = 70;
    std::vector<f32> block(static_cast<usize>(AudioDevice::TapBlockFrames) * 2, 0.0f);
    for (u32 i = 0; i < blocks; ++i)
    {
        block[0] = static_cast<f32>(i);
        device->PublishTapBlock(block, AudioDevice::TapBlockFrames);
    }

    // 63 usable slots (one distinguishes full from empty), so 7 blocks were refused.
    CHECK(device->GetTapOverruns() == 7);

    // Pump(0) mixes nothing, so the drain is the only source of delivered blocks.
    device->Pump(0.0f);
    CHECK(record.Calls == 63);
    CHECK(record.Frames == static_cast<u64>(63) * AudioDevice::TapBlockFrames);

    // The *first* 63 were retained, in order: a full ring drops the incoming block, never an
    // enqueued one.
    REQUIRE(record.FirstSamples.size() == 63);
    bool ordered = true;
    for (usize i = 0; i < record.FirstSamples.size(); ++i)
    {
        ordered = ordered && record.FirstSamples[i] == static_cast<f32>(i);
    }
    CHECK(ordered);

    // Installing a tap resets the count, so a consumer reads losses of its own span only.
    device->SetBlockTap([](std::span<const f32>, u32) {});
    CHECK(device->GetTapOverruns() == 0);
}

TEST_CASE("audio driven: no tap means no ring traffic")
{
    const Unique<AudioDevice> device = MakeNullDevice();

    std::vector<f32> block(static_cast<usize>(AudioDevice::TapBlockFrames) * 2, 0.0f);
    for (u32 i = 0; i < 200; ++i)
    {
        device->PublishTapBlock(block, AudioDevice::TapBlockFrames);
    }
    CHECK(device->GetTapOverruns() == 0);

    // And a tap installed afterwards sees nothing that predates it.
    TapRecord record;
    device->SetBlockTap([&record](std::span<const f32> interleaved, u32 frames)
                        { record.Take(interleaved, frames); });
    device->Pump(0.0f);
    CHECK(record.Calls == 0);
}
