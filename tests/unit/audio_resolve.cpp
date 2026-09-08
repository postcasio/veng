// Audio settings resolve/apply cases, device-free (the null-backend mixer is pure CPU):
//
//  - The default Application::OnResolveAudio is identity: it leaves the pre-filled graph-default
//    gains untouched, so an app that declares no audio schema resolves to the authored defaults.
//  - ApplyAudioSettings pre-fills the active graph's default gains, invokes the resolver, and
//    applies each resulting gain through AudioEngine::SetBusGain, so a stub resolver mapping a
//    chosen scalar onto a bus is observable through GetBusGain — the resolve→apply→mixer path.
//  - An identity apply restores every bus to its graph default (the single-writer property), and a
//    repeated apply is idempotent.
//  - With no audio schema named, the domain is absent: GetAudioSettings() is null.
//
// No Context is constructed: the resolve seam is exposed on an Application built with no window and
// no Run(), and the mixer is a null-backend AudioEngine needing no device.

#include <doctest/doctest.h>

#include <Veng/Application.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBusGraph.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioResolve.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Settings/SettingsSchema.h>
#include <Veng/Settings/SettingsStore.h>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    // A minimal audio schema: one master and one music volume, each a 0..1 scalar defaulting to 1.
    SettingsSchemaData AudioSampleSchema()
    {
        SettingsSchemaData schema;
        SettingsCategory volume;
        volume.Id = "volume";
        volume.Label = "Volume";
        volume.Settings.push_back(SettingsSetting{.Id = "master_volume",
                                                  .Label = "Master",
                                                  .Kind = SettingsSettingKind::Scalar,
                                                  .Min = 0.0f,
                                                  .Max = 1.0f,
                                                  .DefaultValue = 1.0f});
        volume.Settings.push_back(SettingsSetting{.Id = "music_volume",
                                                  .Label = "Music",
                                                  .Kind = SettingsSettingKind::Scalar,
                                                  .Min = 0.0f,
                                                  .Max = 1.0f,
                                                  .DefaultValue = 1.0f});
        schema.Categories.push_back(std::move(volume));
        return schema;
    }

    // Master ⊃ {Music (default gain 0.8), SFX (unity)} — a non-unity default proves the apply
    // pre-fills from the authored default rather than from the runtime's current gain.
    Ref<AudioBusGraph> SampleGraph()
    {
        AudioBusGraphData data;
        data.Buses.push_back(AudioBusDef{.Id = "Master", .Parent = "", .DefaultGain = 1.0f});
        data.Buses.push_back(AudioBusDef{.Id = "Music", .Parent = "Master", .DefaultGain = 0.8f});
        data.Buses.push_back(AudioBusDef{.Id = "SFX", .Parent = "Master", .DefaultGain = 1.0f});
        return AudioBusGraph::Create(std::move(data));
    }

    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

    // Exposes the protected resolve seam and apply core so a test drives them without Run().
    class ResolveApp : public Application
    {
    public:
        using Application::Application;
        using Application::ApplyAudioSettings;
        using Application::OnResolveAudio;
    };

    // A resolver that maps the chosen music volume onto the Music bus through an x² taper.
    class MusicResolveApp final : public ResolveApp
    {
    public:
        using ResolveApp::ResolveApp;

        void OnResolveAudio(const AudioResolveInput& input, AudioResolveOutput& output) override
        {
            const f32 v = input.Settings.GetChosenScalar("music_volume");
            output.SetGain(AudioBuses::Music(), v * v);
        }
    };
}

TEST_CASE("the default OnResolveAudio is identity")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    ResolveApp app{ApplicationInfo{.Name = "audio-resolve-test"}, types, systems};

    const Ref<SettingsSchema> schema = SettingsSchema::Create(AudioSampleSchema());
    const SettingsStore<SettingsChoices> store{
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}}};

    AudioBusGraphData graph;
    graph.Buses.push_back(AudioBusDef{.Id = "Master", .Parent = "", .DefaultGain = 1.0f});
    graph.Buses.push_back(AudioBusDef{.Id = "Music", .Parent = "Master", .DefaultGain = 0.8f});

    AudioResolveOutput output;
    output.Gains.push_back(AudioBusGain{.Bus = AudioBuses::Master(), .Gain = 1.0f});
    output.Gains.push_back(AudioBusGain{.Bus = AudioBuses::Music(), .Gain = 0.8f});

    const AudioResolveOutput before = output;
    const AudioResolveInput input{.Settings = store, .BusGraph = graph};
    app.OnResolveAudio(input, output);

    REQUIRE(output.Gains.size() == before.Gains.size());
    for (usize i = 0; i < output.Gains.size(); ++i)
    {
        CHECK(output.Gains[i].Bus == before.Gains[i].Bus);
        CHECK(output.Gains[i].Gain == doctest::Approx(before.Gains[i].Gain));
    }
}

TEST_CASE("a resolver's gains reach the mixer through ApplyAudioSettings")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    MusicResolveApp app{ApplicationInfo{.Name = "audio-resolve-test"}, types, systems};

    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    engine.ConfigureBusGraph(*SampleGraph());

    const Ref<SettingsSchema> schema = SettingsSchema::Create(AudioSampleSchema());
    SettingsStore<SettingsChoices> store{
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}}};
    store.SetChosenScalar("music_volume", 0.5f);

    app.ApplyAudioSettings(engine, store);

    // The resolver set Music to 0.5² = 0.25; every other bus keeps its pre-filled graph default.
    CHECK(engine.GetBusGain(AudioBuses::Music()) == doctest::Approx(0.25f));
    CHECK(engine.GetBusGain(AudioBuses::SFX()) == doctest::Approx(1.0f));
    CHECK(engine.GetBusGain(AudioBuses::Master()) == doctest::Approx(1.0f));
}

TEST_CASE("an identity apply restores every bus to its graph default")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    ResolveApp app{ApplicationInfo{.Name = "audio-resolve-test"}, types, systems};

    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    engine.ConfigureBusGraph(*SampleGraph());

    const Ref<SettingsSchema> schema = SettingsSchema::Create(AudioSampleSchema());
    const SettingsStore<SettingsChoices> store{
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}}};

    // Perturb a bus away from its default; the identity apply must restore it.
    engine.SetBusGain(AudioBuses::Music(), 0.2f);
    REQUIRE(engine.GetBusGain(AudioBuses::Music()) == doctest::Approx(0.2f));

    app.ApplyAudioSettings(engine, store);

    CHECK(engine.GetBusGain(AudioBuses::Music()) == doctest::Approx(0.8f));
    CHECK(engine.GetBusGain(AudioBuses::SFX()) == doctest::Approx(1.0f));
    CHECK(engine.GetBusGain(AudioBuses::Master()) == doctest::Approx(1.0f));
}

TEST_CASE("ApplyAudioSettings is idempotent")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    MusicResolveApp app{ApplicationInfo{.Name = "audio-resolve-test"}, types, systems};

    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    engine.ConfigureBusGraph(*SampleGraph());

    const Ref<SettingsSchema> schema = SettingsSchema::Create(AudioSampleSchema());
    SettingsStore<SettingsChoices> store{
        SettingsStoreInfo{.Schema = schema.get(), .Types = &types, .ConfigPath = {}}};
    store.SetChosenScalar("music_volume", 0.5f);

    app.ApplyAudioSettings(engine, store);
    const f32 music = engine.GetBusGain(AudioBuses::Music());
    const f32 sfx = engine.GetBusGain(AudioBuses::SFX());

    app.ApplyAudioSettings(engine, store);
    CHECK(engine.GetBusGain(AudioBuses::Music()) == doctest::Approx(music));
    CHECK(engine.GetBusGain(AudioBuses::SFX()) == doctest::Approx(sfx));
}

TEST_CASE("an unset audio schema leaves the domain absent")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    ResolveApp app{ApplicationInfo{.Name = "audio-resolve-test"}, types, systems};

    // No AudioSettingsSchema named and no Run(): the store is never constructed.
    CHECK(app.GetAudioSettings() == nullptr);
}
