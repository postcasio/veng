#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/Level.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/Dsp.h>
#include <Veng/Audio/Reverb.h>
#include <Veng/Log.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Input.h>
#include <Veng/LevelOverlay.h>
#include <Veng/Log.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "MarkerSet.h"

using namespace Veng;

// The primary HUD's view-model: the game-owned data its bindings read. `{Caption}` and `{Level}` in
// the markup resolve their field paths against this reflected struct through the TypeRegistry.
struct TemplateHud
{
    string Caption = "warming up";
    f32 Level = 0.0f;
};

VE_REFLECT(::TemplateHud, 0x0D0C072CE1127CF4ULL)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();

// A snapshot of primary-world state copied into the overlay scene by the overlay's populate hook,
// then read by the overlay's own system to drive its HUD. It is an ordinary reflected component: the
// engine calls the hook once with the fresh overlay scene, the hook attaches this, and no overlay
// system ever reaches back into the primary scene. A copy gives a frozen overlay — the value the
// primary held at open, held for the modal's lifetime even as the primary keeps ticking beneath it.
struct OverlaySnapshot
{
    string Caption = "(none)";
    f32 Level = 0.0f;
};

VE_REFLECT(::OverlaySnapshot, 0x328170442B9DB990ULL)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();

// The dismiss channel back to the opener: the overlay HUD's `onClick` handler raises Requested, and
// the app draining this component each frame closes the overlay. Authored on the HUD entity so it
// exists before the first tick; results cross the overlay boundary only through an explicit
// game-owned channel like this, never by an overlay system touching the primary scene.
struct OverlayControl
{
    bool Requested = false;
};

VE_REFLECT(::OverlayControl, 0xD865A8B4DB6DEA5CULL)
VE_FIELD(Requested)
VE_REFLECT_END();
// The dismiss channel is a view/presentation output: the overlay's driver (a presentation binding)
// writes it and the opener reads it. Tagging it ViewOutput marks it as within the driver boundary —
// a driver may write it, but never a replicated or Sim-input component.
VE_VIEW_OUTPUT(::OverlayControl);

// Drives the overlay level's own HUD as a per-instance presentation driver — named on the overlay
// HUD's GuiOverlay, instantiated by the engine with the document. On instantiate it seeds a
// view-model from the populate-hook snapshot and binds it plus a "Dismiss" handler to the document;
// each frame it mirrors the button's press into the OverlayControl the opener drains. It reads its
// snapshot and drives its HUD entirely within the overlay scene, stamping only the ViewOutput dismiss
// channel — the "modal live scene" runs its own simulation, its HUD bound by its own driver instance.
class TemplateOverlayDriver final : public GuiDriver
{
public:
    void OnInstantiate(Gui::Document& document, Scene& scene, Entity) override
    {
        // Seed the model from the populate-hook snapshot, then bind it plus the dismiss handler to
        // the freshly instantiated document (re-run on any re-instantiate, so the binding survives).
        if (const OverlaySnapshot* snapshot = scene.TryGetFirst<OverlaySnapshot>())
        {
            m_Model = *snapshot;
        }
        m_Context.SetData(m_Model);
        m_Context.SetHandler("Dismiss", [this](Gui::Element&) { m_DismissRequested = true; });
        document.BindContext(&m_Context);
    }

    void OnUpdate(const GuiDriverFrame& frame) override
    {
        // Publish the button's press into the drained channel; the opener reads it and closes.
        if (auto* control = frame.Scene.TryGetFirst<OverlayControl>())
        {
            control->Requested = m_DismissRequested;
        }
    }

private:
    OverlaySnapshot m_Model;
    Gui::BindingContext m_Context;
    bool m_DismissRequested = false;
};

VE_GUI_DRIVER(TemplateOverlayDriver, 0xE9906144475EB699ULL, "Template Overlay");

// The cooked overlay level the Tab key opens as a secondary, simulated overlay. Its own prefab
// authors an input seat, a spinning cube, and an interactive GuiOverlay HUD; its `systems` name the
// builtin input systems plus TemplateOverlaySystem.
constexpr AssetId OverlayLevelId{0x88B360A2DD16632EULL};

// The cooked tuning table and the row this app reads out of it. Structured configuration lives in
// a keyed DataTable validated against its schema at cook time, so the code looks a row up by key
// instead of parsing a blob and re-checking its shape at startup.
constexpr AssetId TuningTableId{0x3FDB8FFFCC00B911ULL};
constexpr i64 TuningRowKey = 20;

// The game-defined marker set cooked by the template's own cook module. It loads through the
// ordinary typed path — Load/LoadSync behind AssetHandle<T> — because the module registered its
// type id and a loader factory; the engine has no compile-time knowledge of it.
constexpr AssetId MarkerSetId{0x4A433D1EA2E5ACF2ULL};

// The component that puts a consumer-defined asset type where a game actually puts one: on a
// prefab, behind a reflected AssetHandle field. Authored on the scene prefab with a plain hex id,
// it is collected as a load-time prefab dependency, loaded through the module's own loader, and
// rehydrated into the spawned component — the path every builtin asset already takes, reached by
// a type the engine has never heard of. The direct LoadSync below covers loading one from code;
// this covers referencing one from data, which is the case that had no exemplar at all.
struct MarkerBeacon
{
    AssetHandle<Template::MarkerSet> Markers;
    string Marker = "overlook";
};

VE_REFLECT(::MarkerBeacon, 0xDEAF7B4317113B8AULL)
VE_FIELD(Markers)
VE_FIELD(Marker)
VE_REFLECT_END();

// The one live-drive field the DemoSynth reads across the thread boundary: a low-pass cutoff the app
// nudges from a clock. It is a trivially-copyable POD so it may ride a GeneratorParams block.
struct DemoParams
{
    f32 Cutoff = 800.0f;
};

// A deliberately simple demonstrator instrument: two Dsp::Oscillators a fifth apart, spread across a
// stereo pair, low-pass filtered with modest resonance, amplitude-shaped by an ADSR, and sent through
// an embedded Reverb — the toolkit's parts wired together end to end in one non-spatial stereo voice.
//
// It is a demonstrator of composition, NOT a synth to reuse. The oscillator count, the interval, the
// routing, and the drive character are an example's arbitrary taste, not an engine opinion — a
// consumer builds the voice *it* wants from the same parts. The engine ships the primitives; where
// they compose into a playable voice is the consumer's call.
//
// Render runs on the real-time mixing thread, so it only latches the param block and ticks the
// primitives — no lock, no allocation, no engine call. The single off-thread allocation is the
// embedded reverb's Prepare, run once before the voice is registered.
class DemoSynth final : public Audio::IAudioGenerator
{
public:
    // Sizes the embedded reverb and seeds the primitives, off the real-time thread. Must run before
    // PlayGenerator hands the mixer the voice. The reverb and the sample-count timings (envelope,
    // cutoff slew) are sized against the standard output rate — the mixer prepares its own master
    // reverb the same way — while oscillator pitch and filter cutoff track the rate Render is handed.
    void Prepare(const u32 sampleRate)
    {
        m_Reverb.Prepare(sampleRate);
        for (Audio::Dsp::Oscillator& osc : m_Osc)
        {
            osc.SetShape(0.8f); // between saw (~2/3) and square (1) — a bright, hollow morph
        }
        for (Audio::Dsp::Filter& filter : m_Filter)
        {
            // Modest resonance: an audible peak, well short of self-oscillation.
            filter.SetResonance(5.0f);
        }
        m_Cutoff.SetValue(DemoParams{}.Cutoff);
        m_Cutoff.SetTime(0.02f, sampleRate);
        m_Env.SetSeconds(0.5f, 0.4f, 0.75f, 0.8f, sampleRate);
        // A sustained drone: rise through attack/decay and hold at sustain for the run.
        m_Env.NoteOn();
    }

    // Publishes new synthesis parameters from the main/View thread.
    void SetParams(const DemoParams& params) { m_Params.Set(params); }

    void Render(f32* out, const u32 frames, const u32 channels, const u32 sampleRate) override
    {
        const DemoParams params = m_Params.Get();
        m_Cutoff.SetTarget(params.Cutoff);
        m_Osc[0].SetFrequency(BaseHz, sampleRate);
        m_Osc[1].SetFrequency(BaseHz * IntervalRatio, sampleRate);

        // Process in sub-blocks bounded by the reverb scratch: one mono send, one stereo wet, per pass.
        u32 done = 0;
        while (done < frames)
        {
            const u32 block = std::min(frames - done, BlockFrames);
            for (u32 i = 0; i < block; ++i)
            {
                const f32 cutoff = m_Cutoff.Tick();
                m_Filter[0].SetCutoff(cutoff, sampleRate);
                m_Filter[1].SetCutoff(cutoff, sampleRate);

                const f32 a = m_Osc[0].Tick();
                const f32 b = m_Osc[1].Tick();
                // Spread the two oscillators: the first leans left, the second right, so the stereo
                // image is audibly wide before the reverb widens it further.
                const f32 gain = m_Env.Tick() * Level;
                const f32 left = m_Filter[0].Tick(a * 0.75f + b * 0.25f).LowPass * gain;
                const f32 right = m_Filter[1].Tick(a * 0.25f + b * 0.75f).LowPass * gain;

                m_Send[i] = (left + right) * 0.5f; // fold to the mono reverb send
                if (channels >= 2)
                {
                    out[(done + i) * channels + 0] = left;
                    out[(done + i) * channels + 1] = right;
                }
                else
                {
                    out[(done + i) * channels] = m_Send[i];
                }
            }

            m_Reverb.ProcessBlock(m_Send.data(), m_WetL.data(), m_WetR.data(), block,
                                  Audio::ReverbParams{.RoomSize = 0.7f,
                                                      .Damping = 0.4f,
                                                      .Width = 1.0f,
                                                      .Quality = Audio::ReverbQuality::Standard});
            if (channels >= 2)
            {
                for (u32 i = 0; i < block; ++i)
                {
                    out[(done + i) * channels + 0] += m_WetL[i] * WetMix;
                    out[(done + i) * channels + 1] += m_WetR[i] * WetMix;
                }
            }
            done += block;
        }
    }

private:
    // The base pitch and the second oscillator's interval above it — a perfect fifth.
    static constexpr f32 BaseHz = 110.0f;
    static constexpr f32 IntervalRatio = 1.5f;
    // The dry output level before the voice's own Gain, and the caller-applied reverb wet mix (the
    // reverb's ProcessBlock leaves ReverbParams::Wet to the caller).
    static constexpr f32 Level = 0.35f;
    static constexpr f32 WetMix = 0.35f;
    // The sub-block size; the mixer never hands Render more than one mixer chunk plus a resample
    // carry, so this bounds the reverb scratch with room to spare.
    static constexpr u32 BlockFrames = 1024;

    Audio::Dsp::Oscillator m_Osc[2];
    Audio::Dsp::Filter m_Filter[2];
    Audio::Dsp::Smoother m_Cutoff;
    Audio::Dsp::Envelope m_Env;
    Audio::Reverb m_Reverb;
    Audio::GeneratorParams<DemoParams> m_Params;

    std::array<f32, BlockFrames> m_Send{};
    std::array<f32, BlockFrames> m_WetL{};
    std::array<f32, BlockFrames> m_WetR{};
};

// The smallest veng game that also authors a HUD and opens a live sub-scene: the bare managed-world
// app (a rotating cube driven entirely by cooked data) grows a minimal Application subclass. Its
// jobs are the primary HUD's data binding (the one thing the engine cannot do from data alone) and
// the lifecycle of a secondary overlay level opened on a key. The primary HUD is authored on an
// entity in the world prefab as a GuiOverlay, so the Viewport owns its load / instantiate / attach.
class TemplateApp final : public Application
{
public:
    // Smoke mode runs the app windowless for a fixed handful of frames and exits 0, so a CI job
    // (the SDK conformance test) can assert the launcher *ran* — loaded its project, spawned its
    // world, and resolved its assets — rather than only that it linked. The template renders no
    // golden, so its correctness signal is the exit status plus what it logged.
    TemplateApp(const ApplicationInfo& info, TypeRegistry& types, SystemRegistry& systems,
                const bool smoke)
        : Application(info, types, systems), m_Smoke(smoke)
    {
    }

    // Runs before ~Application, while the audio engine is still alive: stop the generator voice so the
    // mixer no longer references the borrowed m_Synth before it is destroyed with the app.
    ~TemplateApp() override { GetAudioEngine().StopVoice(m_SynthVoice); }

private:
    // The world is loaded here; find the prefab-authored primary GuiOverlay and bind it the
    // view-model, and load the overlay level's handle so a later Open finds it. The bind is deferred
    // — the overlay applies it when the Viewport instantiates the document — so this runs before the
    // first render with no ordering hole.
    void OnWorldLoaded(WorldInstanceId, Scene& world, ResidencyBatch&) override
    {
        m_Context.SetData(m_Model);
        for (auto [entity, overlay] : world.View<GuiOverlay>())
        {
            overlay.SetContext(&m_Context);
        }

        LoadTuning();

        // Hold the overlay level asset resident so opening it is a spawn, not a load. Open still
        // waits on the spawn's residency (WaitForResidency), accepting the first-open hitch.
        if (const auto level = GetAssetManager().LoadSync<Level>(OverlayLevelId))
        {
            m_OverlayLevel = *level;
        }

        // The custom-asset seam's end-to-end proof: a type the engine does not define, cooked by
        // the game's own importer, resolves through the engine's own typed load path.
        const auto markers = GetAssetManager().LoadSync<Template::MarkerSet>(MarkerSetId);
        VE_ASSERT(markers.has_value(), "template: marker set failed to load: {}",
                  markers ? string{} : markers.error().Detail);
        m_Markers = *markers;

        ReportBeacon(world);
        SetupSynth();
    }

    // Registers the demonstrator instrument as a live stereo, non-spatial voice on the Music bus. The
    // reverb is prepared here, off the mixing thread, before the mixer is ever handed the generator;
    // OnUpdate then drives its cutoff live through the param block. It runs in every mode (silent under
    // the headless null device the smoke path uses), so the voice exists whenever the app does.
    void SetupSynth()
    {
        m_Synth.Prepare(SynthSampleRate);
        m_SynthVoice = GetAudioEngine().PlayGenerator(
            &m_Synth,
            Audio::GeneratorVoiceParams{
                .Bus = Audio::AudioBus::Music, .Spatial = false, .Channels = 2, .Gain = 0.5f});
    }

    // Reads the prefab-authored reference to the game-defined asset and reports what it resolved
    // to. Nothing here loads anything: the prefab named the marker set through a reflected
    // AssetHandle, so the engine had already loaded it as a load-time dependency and rehydrated
    // the field before this entity existed. The line it prints is what the SDK conformance test
    // reads back off the launcher's stdout — the assertion that the seam ran, not merely built.
    void ReportBeacon(Scene& world)
    {
        usize found = 0;
        for (auto [entity, beacon] : world.View<MarkerBeacon>())
        {
            VE_ASSERT(beacon.Markers.IsLoaded(),
                      "template: the prefab-authored marker-set handle did not resolve");

            const Template::Marker* const marker = beacon.Markers->Find(beacon.Marker);
            VE_ASSERT(marker != nullptr, "template: marker set declares no '{}' marker",
                      beacon.Marker);
            Log::Info("template: MarkerBeacon resolved {} markers, '{}' at ({}, {}, {})",
                      beacon.Markers->Markers.size(), beacon.Marker, marker->Position.x,
                      marker->Position.y, marker->Position.z);
            ++found;
        }
        VE_ASSERT(found == 1, "template: expected exactly one prefab-authored MarkerBeacon, saw {}",
                  found);
    }

    // Feed the primary HUD's bound fields, toggle the overlay on Tab, and — while it is open — tick it
    // and drain its dismiss channel. The Viewport's per-frame overlay drive re-resolves the primary
    // bindings and composites the HUD, so the game writes no layout or attach code.
    void OnUpdate(const f32 delta) override
    {
        if (m_Smoke && ++m_Frame >= SmokeFrames)
        {
            RequestExit();
            return;
        }

        // Sweep the synth's low-pass cutoff from a clock — the sanctioned live-parameter seam, a step
        // eased by the synth's own Smoother so it never zippers. This is the resonant filter sweep a
        // listen would judge.
        m_SynthClock += delta;
        m_Synth.SetParams(DemoParams{.Cutoff = 800.0f + 500.0f * std::sin(m_SynthClock * 0.5f)});

        m_Model.Caption =
            fmt::format("{} — {:.0f} fps", m_TuningLabel, delta > 0.0f ? 1.0f / delta : 0.0f);
        m_Model.Level = std::clamp(delta > 0.0f ? (1.0f / delta) / 120.0f : 0.0f, 0.0f, 1.0f);
        m_Context.Invalidate();

        if (GetInput().WasKeyPressed(Key::Tab))
        {
            if (m_Overlay)
            {
                m_Overlay.reset();
            }
            else
            {
                OpenOverlay();
            }
        }

        if (m_Overlay)
        {
            // The engine ticks the overlay's simulation and pushes its camera each frame (it opens an
            // owned world on Open), so the opener writes no per-frame overlay code — only the dismiss
            // drain. The overlay system published the HUD button's click into OverlayControl, which
            // the opener owns and drains here. (Tab, above, is the other dismissal.)
            const OverlayControl* const control =
                m_Overlay->GetScene().TryGetFirst<OverlayControl>();
            if (control != nullptr && control->Requested)
            {
                m_Overlay.reset();
            }
        }
    }

    // Reads the app's tuning row out of the cooked table: one key lookup, then accessors resolved
    // once each. The fixed-size columns come back through the zero-copy view; the string column is
    // read out of the row itself. The asset-handle cell yields a bare AssetId — the table holds no
    // handle to the icon texture and never loads it, so reading the row costs nothing beyond the
    // row itself.
    void LoadTuning()
    {
        const AssetResult<AssetHandle<DataTable>> tuning =
            GetAssetManager().LoadSync<DataTable>(TuningTableId);
        if (!tuning)
        {
            return;
        }

        const optional<u32> row = (*tuning)->FindRow(TuningRowKey);
        if (!row)
        {
            return;
        }

        const TableColumn<f32> spinSpeed = (*tuning)->GetColumn<f32>("spinSpeed");
        const TableColumn<AssetId> icon = (*tuning)->GetAssetIdColumn("icon");
        const Result<std::string_view> label = (*tuning)->GetStringCell(*row, "label");
        if (!label)
        {
            Log::Error("Template: {}", label.error());
            return;
        }

        m_TuningLabel = string(*label);
        Log::Info("Template: tuning row {} is '{}' at {:.2f} rad/s, icon {:#018x}", TuningRowKey,
                  m_TuningLabel, spinSpeed[*row], icon[*row].Value);
    }

    // Opens the overlay level over the running frame: a fresh owned world simulated concurrently, its
    // own seat taking input while the managed world's is suspended, and — naming the managed world as
    // the covered world — that world's simulation frozen for the modal's lifetime. The populate hook
    // copies a snapshot of the primary HUD's state into the overlay scene before it starts.
    void OpenOverlay()
    {
        if (!m_OverlayLevel.Id().IsValid())
        {
            return;
        }

        m_Overlay = LevelOverlay::Open(
            *this,
            LevelOverlayInfo{
                .Source = m_OverlayLevel,
                .CoveredWorld = GetManagedWorldId(),
                .WaitForResidency = true,
                .Populate =
                    [this](Scene& scene)
                {
                    const Entity entity = scene.CreateEntity();
                    scene.Add<OverlaySnapshot>(entity, OverlaySnapshot{.Caption = m_Model.Caption,
                                                                       .Level = m_Model.Level});
                },
            });
    }

    TemplateHud m_Model;
    Gui::BindingContext m_Context;

    // The label read out of the tuning table at startup, shown alongside the frame rate.
    string m_TuningLabel = "untuned";

    // The overlay level asset, held resident from OnWorldLoaded, and the live handle while it is open.
    AssetHandle<Level> m_OverlayLevel;
    std::optional<LevelOverlay> m_Overlay;

    // The game-defined asset, held resident for the app's lifetime.
    AssetHandle<Template::MarkerSet> m_Markers;

    // The demonstrator instrument, its live voice handle, and the clock driving its cutoff sweep. The
    // standard output rate the reverb and envelope timings are sized against (the mixer runs at it).
    static constexpr u32 SynthSampleRate = 48000;
    DemoSynth m_Synth;
    Audio::VoiceHandle m_SynthVoice;
    f32 m_SynthClock = 0.0f;

    // Enough frames for the world load, the first spawn, and a couple of rendered frames to
    // settle before smoke mode exits.
    static constexpr u32 SmokeFrames = 8;

    const bool m_Smoke = false;
    u32 m_Frame = 0;
};

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<TemplateHud>();
    host->Types.Register<OverlaySnapshot>();
    host->Types.Register<OverlayControl>();
    // Registered like any other component; its AssetHandle field needs no special treatment
    // beyond the type's own HandleFieldType registration below.
    host->Types.Register<MarkerBeacon>();
    // The overlay HUD's presentation binding is a per-instance driver named on its GuiOverlay, not a
    // per-world system: the engine instantiates it with the document and drives it each frame.
    if (host->Drivers != nullptr)
    {
        host->Drivers->Register<TemplateOverlayDriver>();
    }

    // The game-defined asset type registers here and nowhere else: this entry is reachable from
    // every host (launcher, cooker, editor), so one registration serves all three. The cook module
    // contributes only the importer — registering the id from both seams would deliver it twice in
    // the editor, where both images load, and a duplicate id is fatal.
    // HandleFieldType is the third and last thing a game does for a referenceable type: it pairs
    // the reflected AssetHandle<MarkerSet> leaf back to this asset type, which is what lets the
    // prefab loader collect MarkerBeacon::Markers as a dependency, the cooker type-check the id
    // authored there, and the editor offer a picker for it. Without it, a prefab carrying that
    // component is a located cook error.
    host->AssetTypes.Register(
        AssetTypeInfo{.Id = Template::MarkerSetAssetType,
                      .Name = Template::MarkerSetTypeName,
                      .DisplayName = "Marker Set",
                      .Glyph = "MRK",
                      .HandleFieldType = TypeIdOf<AssetHandle<Template::MarkerSet>>()});
    host->AssetLoaders.Register(Template::MarkerSetAssetType, []
                                { return Unique<AssetLoader>(new Template::MarkerSetLoader()); });

    // The registries are host-owned and outlive this module, so the factory captures them and
    // points the app's AssetManager at them — the ApplicationInfo fields are how a module-defined
    // loader reaches the running manager.
    // Smoke mode: no window or swapchain, a fixed handful of frames, then exit — the display-free
    // CI path the SDK conformance test drives the launcher through.
    const bool smoke = std::getenv("TEMPLATE_SMOKE") != nullptr;

    host->App.RegisterApplication(
        [smoke, assetTypes = &host->AssetTypes,
         assetLoaders = &host->AssetLoaders](TypeRegistry& types, SystemRegistry& systems)
        {
            return Unique<Application>(new TemplateApp(
                ApplicationInfo{
                    .Name = "Template",
                    .HeadlessExtent = {1280, 720},
                    .WindowInfo =
                        {
                            .Extent = {1280, 720},
                            .Title = "veng — Template",
                        },
                    .Headless = smoke,
                    // The engine owns the primary viewport (its SceneRenderer + the gather +
                    // composite tail) and drives the managed world: it reads the cooked project,
                    // mounts its packs, loads the startup level, ticks the simulation, and pushes
                    // the resolved camera each frame. The subclass adds the HUD binding and the
                    // secondary overlay level.
                    .ManagedViewport = ManagedViewportInfo{},
                    .World = GameWorldInfo{.Project = "project.vengproj"},
                    .AssetTypes = assetTypes,
                    .AssetLoaders = assetLoaders,
                },
                types, systems, smoke));
        });
}

VE_EXPORT_MODULE_ABI()
