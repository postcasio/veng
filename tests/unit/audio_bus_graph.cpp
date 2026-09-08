// The data-driven audio bus graph: the roots-only default reproduces the historical five-bus mix,
// an authored graph composes gain down its tree, per-bus DSP is leaf-only, the flatten is
// deterministic, an unknown bus falls back to Master, and validation rejects a malformed graph to
// the default. All provable on the null device — the whole fold is pure CPU.

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
    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

    Ref<AudioBuffer> ConstantMono(f32 value, u32 frames)
    {
        const std::vector<f32> samples(frames, value);
        return AudioBuffer::Create(samples, 1, 48000);
    }

    AudioBusDef Bus(string id, string parent, f32 gain = 1.0f)
    {
        return AudioBusDef{.Id = std::move(id), .Parent = std::move(parent), .DefaultGain = gain};
    }

    // Renders one block and returns the first output frame's left channel — the value the gain and
    // pan arithmetic reduces to for a hard-left constant voice.
    f32 RenderFirstLeft(AudioDevice& device, AudioEngine& engine)
    {
        engine.Publish();
        constexpr u32 frames = 128;
        std::vector<f32> output(static_cast<usize>(frames) * device.GetChannels(), 0.0f);
        device.RenderBlock(output, frames);
        return output[0];
    }
}

TEST_CASE("the default graph reproduces the historical five-bus mix")
{
    // A unit voice on SFX, hard left, at unity everywhere reduces to exactly 1.0 on the left and
    // silence on the right — the same value the fixed enum mixer produced, asserted absolutely.
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    engine.AddVoice(
        ConstantMono(1.0f, 64),
        VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    engine.Publish();

    constexpr u32 frames = 128;
    std::vector<f32> output(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
    device->RenderBlock(output, frames);

    CHECK(output[0] == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(output[1] == doctest::Approx(0.0f).epsilon(0.001));
    // The default graph exposes exactly the five well-known roots.
    CHECK(engine.GetBusName(AudioBuses::Master()) == "Master");
    CHECK(engine.GetBusName(AudioBuses::Music()) == "Music");
    CHECK(engine.GetBusName(AudioBuses::SFX()) == "SFX");
    CHECK(engine.GetBusName(AudioBuses::UI()) == "UI");
    CHECK(engine.GetBusName(AudioBuses::Ambience()) == "Ambience");
}

TEST_CASE("gain composes down the tree")
{
    // Master ⊃ SFX ⊃ {A, B}: a gain on the SFX parent scales both leaves, a leaf gain scales only
    // itself, and the master gain scales everything. Composition is the fold's consequence.
    AudioBusGraphData data;
    data.Buses = {Bus("Master", ""), Bus("SFX", "Master"), Bus("A", "SFX"), Bus("B", "SFX")};

    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    engine.ConfigureBusGraph(*AudioBusGraph::Create(data));

    const VoiceHandle a =
        engine.AddVoice(ConstantMono(1.0f, 64),
                        VoiceParams{.Bus = BusId{"A"}, .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    engine.AddVoice(ConstantMono(1.0f, 64),
                    VoiceParams{.Bus = BusId{"B"}, .Gain = 1.0f, .Pan = -1.0f, .Loop = true});

    // Both leaves at unity: A and B each contribute 1.0 to the left → 2.0.
    CHECK(RenderFirstLeft(*device, engine) == doctest::Approx(2.0f).epsilon(0.001));

    // Halving the SFX parent halves both children → 1.0.
    engine.SetBusGain(BusId{"SFX"}, 0.5f);
    CHECK(RenderFirstLeft(*device, engine) == doctest::Approx(1.0f).epsilon(0.001));

    // Silencing leaf A leaves B (0.5 through SFX) alone → 0.5.
    engine.SetBusGain(BusId{"A"}, 0.0f);
    CHECK(RenderFirstLeft(*device, engine) == doctest::Approx(0.5f).epsilon(0.001));

    // The master gain scales the whole mix → 0.25.
    engine.SetBusGain(AudioBuses::Master(), 0.5f);
    CHECK(RenderFirstLeft(*device, engine) == doctest::Approx(0.25f).epsilon(0.001));

    static_cast<void>(a);
}

TEST_CASE("per-bus DSP is leaf-only")
{
    // A lowpass or reverb-send set on a bus with children is ignored (stored value stays 0); the
    // same set on a leaf bus takes effect.
    AudioBusGraphData data;
    data.Buses = {Bus("Master", ""), Bus("SFX", "Master"), Bus("A", "SFX")};

    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    engine.ConfigureBusGraph(*AudioBusGraph::Create(data));

    // SFX has a child (A), so it is non-leaf: DSP is ignored.
    engine.SetBusLowpassCutoff(BusId{"SFX"}, 800.0f);
    engine.SetBusReverbSend(BusId{"SFX"}, 0.5f);
    CHECK(engine.GetBusLowpassCutoff(BusId{"SFX"}) == doctest::Approx(0.0f));
    CHECK(engine.GetBusReverbSend(BusId{"SFX"}) == doctest::Approx(0.0f));

    // A is a leaf: DSP is honoured.
    engine.SetBusLowpassCutoff(BusId{"A"}, 800.0f);
    engine.SetBusReverbSend(BusId{"A"}, 0.5f);
    CHECK(engine.GetBusLowpassCutoff(BusId{"A"}) == doctest::Approx(800.0f));
    CHECK(engine.GetBusReverbSend(BusId{"A"}) == doctest::Approx(0.5f));

    // A low cutoff on the leaf attenuates the one-pole's opening transient: the first output sample
    // is far below the steady 1.0 a bypassed bus would pass. A constant DC voice through a non-leaf
    // (ignored) lowpass would show no such attenuation.
    engine.AddVoice(ConstantMono(1.0f, 64),
                    VoiceParams{.Bus = BusId{"A"}, .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    engine.SetBusLowpassCutoff(BusId{"A"}, 100.0f);
    CHECK(RenderFirstLeft(*device, engine) < 0.2f);
}

TEST_CASE("an unknown bus falls back to Master")
{
    // A voice on an id the active graph does not declare routes to Master, so it still plays; the
    // readout resolves the id to Master's name.
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    engine.AddVoice(ConstantMono(1.0f, 64),
                    VoiceParams{.Bus = BusId{"Engines"}, .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    // Master is unity, so the fallback plays at full level.
    CHECK(RenderFirstLeft(*device, engine) == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(engine.GetBusName(BusId{"Engines"}) == "Master");
    // ResolveBus of an absent name returns the Master id.
    CHECK(engine.ResolveBus("Engines") == AudioBuses::Master());
    CHECK(engine.ResolveBus("SFX") == AudioBuses::SFX());
}

TEST_CASE("the flatten is deterministic")
{
    // Two devices configured with the same graph mix a given voice set identically — the child-
    // before-parent order the fold depends on is a pure function of the graph.
    AudioBusGraphData data;
    data.Buses = {Bus("Master", ""), Bus("SFX", "Master"), Bus("A", "SFX", 0.7f),
                  Bus("B", "SFX", 0.3f)};

    const Unique<AudioDevice> d0 = MakeNullDevice();
    const Unique<AudioDevice> d1 = MakeNullDevice();
    d0->GetEngine().ConfigureBusGraph(*AudioBusGraph::Create(data));
    d1->GetEngine().ConfigureBusGraph(*AudioBusGraph::Create(data));

    for (AudioEngine* engine : {&d0->GetEngine(), &d1->GetEngine()})
    {
        engine->AddVoice(ConstantMono(1.0f, 64),
                         VoiceParams{.Bus = BusId{"A"}, .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    }
    CHECK(RenderFirstLeft(*d0, d0->GetEngine()) ==
          doctest::Approx(RenderFirstLeft(*d1, d1->GetEngine())));
}

TEST_CASE("graph validation")
{
    const auto valid = [](std::vector<AudioBusDef> buses)
    {
        AudioBusGraphData data;
        data.Buses = std::move(buses);
        return ValidateAudioBusGraph(data).has_value();
    };

    // A well-formed game-shaped graph: the roots plus an SFX subtree.
    CHECK(valid({Bus("Master", ""), Bus("Music", "Master"), Bus("SFX", "Master"),
                 Bus("Engines", "SFX"), Bus("Weapons", "SFX")}));

    // Empty graph.
    CHECK_FALSE(valid({}));
    // Root not named Master.
    CHECK_FALSE(valid({Bus("Root", ""), Bus("SFX", "Root")}));
    // Two roots.
    CHECK_FALSE(valid({Bus("Master", ""), Bus("Other", "")}));
    // Dangling parent.
    CHECK_FALSE(valid({Bus("Master", ""), Bus("SFX", "Nope")}));
    // A cycle among non-root buses (no reachable root path).
    CHECK_FALSE(valid({Bus("Master", ""), Bus("A", "B"), Bus("B", "A")}));
    // A duplicate bus id.
    CHECK_FALSE(valid({Bus("Master", ""), Bus("SFX", "Master"), Bus("SFX", "Master")}));

    // Over-wide: more than MaxBuses.
    std::vector<AudioBusDef> wide{Bus("Master", "")};
    for (u32 i = 0; i < MaxBuses; ++i)
    {
        wide.push_back(Bus(fmt::format("b{}", i), "Master"));
    }
    CHECK_FALSE(valid(std::move(wide)));

    // Over-deep: a chain longer than MaxBusDepth.
    std::vector<AudioBusDef> deep{Bus("Master", "")};
    string parent = "Master";
    for (u32 i = 0; i <= MaxBusDepth; ++i)
    {
        const string name = fmt::format("d{}", i);
        deep.push_back(Bus(name, parent));
        parent = name;
    }
    CHECK_FALSE(valid(std::move(deep)));
}

TEST_CASE("an invalid graph is rejected to the roots-only default")
{
    // ConfigureBusGraph with a malformed graph keeps the engine on the roots-only default rather
    // than adopting it, so the well-known roots still resolve.
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    AudioBusGraphData bad;
    bad.Buses = {Bus("Root", ""), Bus("SFX", "Root")}; // root is not Master
    engine.ConfigureBusGraph(*AudioBusGraph::Create(bad));

    CHECK(engine.GetBusName(AudioBuses::SFX()) == "SFX");
    CHECK(engine.GetBusName(AudioBuses::Master()) == "Master");
}
