// ConfigureBusGraph adopted at boot under a real (Auto-backend) audio device: the republish that
// recomputes the flattened table into the triple-buffered snapshot does not disturb the mixer, and
// a gain set after adoption takes effect. On a host with no audio hardware the Auto backend falls
// back to the null device and the same control-thread assertions hold — the point is that the
// adopt + publish path runs cleanly whether or not a real callback thread is draining the snapshot.

#include <doctest/doctest.h>

#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioBusGraph.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/Voice.h>

#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    AudioBusDef Bus(string id, string parent, f32 gain = 1.0f)
    {
        return AudioBusDef{.Id = std::move(id), .Parent = std::move(parent), .DefaultGain = gain};
    }
}

TEST_CASE("ConfigureBusGraph republishes cleanly under a device")
{
    const Unique<AudioDevice> device =
        AudioDevice::Create(AudioDeviceInfo{.Backend = AudioBackend::Auto});
    AudioEngine& engine = device->GetEngine();

    // Adopt a game-shaped graph before any meaningful mixing.
    AudioBusGraphData data;
    data.Buses = {Bus("Master", ""), Bus("Music", "Master"), Bus("SFX", "Master"),
                  Bus("Engines", "SFX", 0.8f), Bus("Weapons", "SFX")};
    engine.ConfigureBusGraph(*AudioBusGraph::Create(data));

    // A voice on a game-defined leaf plays; a couple of pumps drive publish/consume without a
    // torn frame (the triple buffer isolates the RT reader from these writes).
    const std::vector<f32> samples(4800, 0.25f);
    const Ref<AudioBuffer> clip = AudioBuffer::Create(samples, 1, device->GetSampleRate());
    const VoiceHandle voice =
        engine.AddVoice(clip, VoiceParams{.Bus = BusId{"Engines"}, .Gain = 1.0f, .Loop = true});
    CHECK(voice.IsValid());

    for (int i = 0; i < 4; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }

    // The default gain seeded from the graph, and a gain set after adoption, both take effect.
    CHECK(engine.GetBusGain(BusId{"Engines"}) == doctest::Approx(0.8f));
    engine.SetBusGain(BusId{"Weapons"}, 0.5f);
    engine.SetBusGain(AudioBuses::Master(), 0.75f);
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetBusGain(BusId{"Weapons"}) == doctest::Approx(0.5f));
    CHECK(engine.GetBusGain(AudioBuses::Master()) == doctest::Approx(0.75f));

    // The readout resolves game-defined ids to their authored names.
    CHECK(engine.GetBusName(BusId{"Engines"}) == "Engines");
    CHECK(engine.GetBusName(BusId{"Weapons"}) == "Weapons");

    engine.StopVoice(voice);
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("StopVoice returns on a driven hardware device")
{
    const Unique<AudioDevice> device =
        AudioDevice::Create(AudioDeviceInfo{.Backend = AudioBackend::Auto});
    if (device->IsNull())
    {
        MESSAGE("skipped: no hardware audio device in this session");
        return;
    }
    AudioEngine& engine = device->GetEngine();

    // Driving the device stops its callback thread; the mixer then runs only inside Pump, on this
    // thread. A stop that waited for the callback thread to consume the removal would wait forever.
    device->SetDriven(true);
    REQUIRE(device->IsDriven());

    const std::vector<f32> samples(4800, 0.25f);
    const Ref<AudioBuffer> clip = AudioBuffer::Create(samples, 1, device->GetSampleRate());
    const VoiceHandle voice =
        engine.AddVoice(clip, VoiceParams{.Bus = AudioBuses::Master(), .Gain = 1.0f, .Loop = true});
    CHECK(voice.IsValid());
    device->Pump(1.0f / 60.0f);

    // The property is that this call returns at all; it did not before the driven device took the
    // inline-mix path. The pump after it reaps the voice.
    engine.StopVoice(voice);
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetActiveVoiceCount() == 0);

    device->SetDriven(false);
    CHECK_FALSE(device->IsDriven());
}
