#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/Level.h>
#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/Dsp.h>
#include <Veng/Audio/Reverb.h>
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
#include <Veng/Renderer/ModelPortrait.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

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

// A snapshot of primary-world state the overlay's HUD shows. It sits on the entity requesting the
// overlay, which names itself as the request's Seed, so the engine copies it into the fresh overlay
// scene before that scene starts and no overlay code ever reaches back into the primary scene. A copy
// gives a frozen overlay — the value the primary held at open, held for the modal's lifetime.
struct OverlaySnapshot
{
    string Caption = "(none)";
    f32 Level = 0.0f;
};

VE_REFLECT(::OverlaySnapshot, 0x328170442B9DB990ULL)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();

// Drives the overlay level's own HUD as a per-instance presentation driver — named on the overlay
// HUD's GuiOverlay, instantiated by the engine with the document. On instantiate it seeds a
// view-model from the seeded snapshot and binds it plus a "Dismiss" handler to the document; a press
// stamps an ExitRequest in the overlay's own scene, which ends the overlay rather than the
// application. Everything it touches is in the overlay scene.
class TemplateOverlayDriver final : public GuiDriver
{
public:
    void OnInstantiate(const GuiDriverContext& context) override
    {
        // Seed the model from the seeded snapshot, then bind it plus the dismiss handler to the
        // freshly instantiated document (re-run on any re-instantiate, so the binding survives).
        if (const OverlaySnapshot* snapshot = context.Scene.TryGetFirst<OverlaySnapshot>())
        {
            m_Model = *snapshot;
        }
        m_Context.SetData(m_Model);
        m_Context.SetHandler("Dismiss", [this](Gui::Element&) { m_DismissRequested = true; });
        context.Document.BindContext(&m_Context);
    }

    void OnUpdate(const GuiDriverFrame& frame) override
    {
        if (m_DismissRequested && frame.Scene.TryGetFirst<ExitRequest>() == nullptr)
        {
            frame.Scene.Add<ExitRequest>(frame.Owner);
        }
        m_DismissRequested = false;
    }

private:
    OverlaySnapshot m_Model;
    Gui::BindingContext m_Context;
    bool m_DismissRequested = false;
};

VE_GUI_DRIVER(TemplateOverlayDriver, 0xE9906144475EB699ULL, "Template Overlay");

// Paints the primary HUD's model portrait: a ModelPortrait on the HUD's own entity, which the engine
// renders offscreen ahead of the viewport. Each frame the driver hands the render to the HUD's Image,
// or returns the Image to nothing while the portrait is not ready, so the HUD binds no texture the
// portrait has handed back.
class TemplateHudDriver final : public GuiDriver
{
public:
    void OnInstantiate(const GuiDriverContext& context) override
    {
        m_Portrait = context.Document.FindById("portrait");
    }

    void OnUpdate(const GuiDriverFrame& frame) override
    {
        if (m_Portrait == nullptr)
        {
            return;
        }
        const auto* portrait =
            std::as_const(frame.Scene).TryGet<Renderer::ModelPortrait>(frame.Owner);
        const Renderer::ModelPortraitOutput output =
            portrait != nullptr ? portrait->GetOutput() : Renderer::ModelPortraitOutput{};
        if (!output.Ready)
        {
            frame.Document.ClearImageTexture(*m_Portrait);
            return;
        }
        frame.Document.SetImageTexture(*m_Portrait, output.Color, output.Sampler, output.Extent);
        if (!m_Reported)
        {
            Log::Info("template: model portrait painted into the HUD at {}x{}", output.Extent.x,
                      output.Extent.y);
            m_Reported = true;
        }
    }

    void OnDetach(const GuiDriverContext&) override { m_Portrait = nullptr; }

private:
    Gui::Element* m_Portrait = nullptr;
    bool m_Reported = false;
};

VE_GUI_DRIVER(TemplateHudDriver, 0x4799243B384791D3ULL, "Template HUD");

// The embedded emblem component's own view-model: a counter the component owns and no host knows
// about. The emblem fragment's `{Beats}` binding resolves this through the component's scoped
// context, not the primary HUD's.
struct EmblemModel
{
    i32 Beats = 0;
};

VE_REFLECT(::EmblemModel, 0xEFD2B0E098C69502ULL)
VE_FIELD(Beats)
VE_REFLECT_END();

// The embedded emblem's presentation driver, named on the primary HUD's `<Component driver="…">`.
// It proves an embedded component carries its own behaviour: the engine instantiates one driver per
// boundary, each owning an independent EmblemModel it advances from its own clock and binds *scoped
// to its boundary's subtree*, so the emblem's `{Beats}` reads the component's view-model while the
// surrounding HUD's `{Caption}`/`{Level}` read the host's — two contexts on one document, the
// component's driven with no host knowledge of its internals.
class EmblemDriver final : public GuiDriver
{
public:
    void OnInstantiate(const GuiDriverContext& context) override
    {
        // Bind the component's own view-model scoped to its boundary's subtree; the emblem's
        // `{Beats}` resolves against this rather than the host HUD's context.
        m_Context.SetData(m_Model);
        context.Document.BindContext(context.Document.GetHandle(context.Root), &m_Context);
    }

    void OnUpdate(const GuiDriverFrame& frame) override
    {
        // Advance the owned counter from the component's own clock and republish it — a value the
        // component mutates itself, independent of anything the host drives.
        m_Elapsed += frame.Delta;
        if (const i32 beats = static_cast<i32>(m_Elapsed * 2.0f); beats != m_Model.Beats)
        {
            m_Model.Beats = beats;
            m_Context.Invalidate();
        }
    }

private:
    EmblemModel m_Model;
    Gui::BindingContext m_Context;
    f32 m_Elapsed = 0.0f;
};

VE_GUI_DRIVER(EmblemDriver, 0xC8CA6B412AB63897ULL, "Emblem");

// The cooked overlay level the Tab key opens as a secondary, simulated overlay. Its own prefab
// authors an input seat, a spinning cube, and an interactive GuiOverlay HUD; its `systems` name the
// builtin input systems and the builtin constant-motion system that spins the cube.
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
// toggling a secondary overlay level on a key. The primary HUD is authored on an entity in the world
// prefab as a GuiOverlay, so the Viewport owns its load / instantiate / attach.
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

private:
    // The world is loaded here; find the prefab-authored primary GuiOverlay and bind it the
    // view-model, and load the overlay level's handle so a later request finds it resident. The bind
    // is deferred — the overlay applies it when the Viewport instantiates the document — so this runs
    // before the first render with no ordering hole.
    void OnWorldLoaded(WorldInstanceId, Scene& world, ResidencyBatch&) override
    {
        m_Context.SetData(m_Model);
        for (auto [entity, overlay] : world.View<GuiOverlay>())
        {
            overlay.SetContext(&m_Context);
        }

        LoadTuning();

        // Hold the overlay level asset resident so opening it is a spawn, not a load. The open still
        // waits on the spawn's residency (WaitForResidency), accepting the first-open hitch.
        if (const auto level = GetAssetManager().LoadSync<Level>(OverlayLevelId))
        {
            m_OverlayLevel = *level;
        }

        // The entity the Tab key's overlay request goes on.
        m_OverlayOpener = world.CreateEntity();
        world.Add<Name>(m_OverlayOpener).Value = "Overlay Opener";

        // The custom-asset seam's end-to-end proof: a type the engine does not define, cooked by
        // the game's own importer, resolves through the engine's own typed load path.
        const auto markers = GetAssetManager().LoadSync<Template::MarkerSet>(MarkerSetId);
        VE_ASSERT(markers.has_value(), "template: marker set failed to load: {}",
                  markers ? string{} : markers.error().Detail);
        m_Markers = *markers;

        ReportBeacon(world);
        SetupSynth(world);
    }

    // Plays the demonstrator instrument from an AudioSource on an entity of the world, so it belongs
    // to the scene: it holds while the world is paused and stops with it. The generator is
    // runtime-only, so it is attached here rather than authored; its reverb is prepared first, off
    // the mixing thread, and OnUpdate drives its cutoff live through the param block. It runs in
    // every mode (silent under the headless null device the smoke path uses).
    void SetupSynth(Scene& world)
    {
        m_Synth->Prepare(SynthSampleRate);
        const Entity synth = world.CreateEntity();
        world.Add<Name>(synth).Value = "Demo Synth";
        world.Add<Transform>(synth, Transform{});
        world.Add<AudioSource>(synth, AudioSource{.Bus = string(Audio::AudioBuses::MusicName),
                                                  .Gain = 0.5f,
                                                  .Playing = true,
                                                  .Spatial = false,
                                                  .Channels = 2,
                                                  .Generator = m_Synth});
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

    // Feed the primary HUD's bound fields and toggle the overlay on Tab. The Viewport's per-frame
    // overlay drive re-resolves the primary bindings and composites the HUD, so the game writes no
    // layout or attach code.
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
        m_Synth->SetParams(DemoParams{.Cutoff = 800.0f + 500.0f * std::sin(m_SynthClock * 0.5f)});

        m_Model.Caption =
            fmt::format("{} — {:.0f} fps", m_TuningLabel, delta > 0.0f ? 1.0f / delta : 0.0f);
        m_Model.Level = std::clamp(delta > 0.0f ? (1.0f / delta) / 120.0f : 0.0f, 0.0f, 1.0f);
        m_Context.Invalidate();

        if (GetInput().WasKeyPressed(Key::Tab))
        {
            ToggleOverlay();
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

    // Requests the overlay level over the managed world, or withdraws the request. The engine opens
    // it at the next frame: a fresh world simulated concurrently, its own seat taking input while the
    // managed world's is suspended, and the managed world paused for the modal's lifetime. The
    // request's entity carries a snapshot of the primary HUD's state and seeds the overlay with it.
    // The overlay's own Dismiss button ends it the same way the key does, removing the request.
    void ToggleOverlay()
    {
        World* const world = GetWorldRunner().ResolveWorld(GetManagedWorldId());
        if (world == nullptr || m_OverlayOpener.IsNull() || !m_OverlayLevel.Id().IsValid())
        {
            return;
        }
        Scene& scene = world->GetScene();
        if (std::as_const(scene).TryGet<LevelOverlay>(m_OverlayOpener) != nullptr)
        {
            (void)scene.Remove<LevelOverlay>(m_OverlayOpener);
            return;
        }
        const OverlaySnapshot snapshot{.Caption = m_Model.Caption, .Level = m_Model.Level};
        if (auto* const held = scene.TryGet<OverlaySnapshot>(m_OverlayOpener))
        {
            *held = snapshot;
        }
        else
        {
            scene.Add<OverlaySnapshot>(m_OverlayOpener, snapshot);
        }
        scene.Add<LevelOverlay>(m_OverlayOpener, LevelOverlay{.Source = m_OverlayLevel,
                                                              .PauseOpener = true,
                                                              .WaitForResidency = true,
                                                              .Seed = m_OverlayOpener});
    }

    TemplateHud m_Model;
    Gui::BindingContext m_Context;

    // The label read out of the tuning table at startup, shown alongside the frame rate.
    string m_TuningLabel = "untuned";

    // The overlay level asset, held resident from OnWorldLoaded, and the managed-world entity its
    // request goes on.
    AssetHandle<Level> m_OverlayLevel;
    Entity m_OverlayOpener = Entity::Null;

    // The game-defined asset, held resident for the app's lifetime.
    AssetHandle<Template::MarkerSet> m_Markers;

    // The demonstrator instrument (kept here to drive its params; its source plays it) and the clock
    // driving its cutoff sweep. The standard output rate the reverb and envelope timings are sized against (the mixer runs at it).
    static constexpr u32 SynthSampleRate = 48000;
    Ref<DemoSynth> m_Synth = CreateRef<DemoSynth>();
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
    host->Types.Register<EmblemModel>();
    // Registered like any other component; its AssetHandle field needs no special treatment
    // beyond the type's own HandleFieldType registration below.
    host->Types.Register<MarkerBeacon>();
    // The overlay HUD's presentation binding is a per-instance driver named on its GuiOverlay, not a
    // per-world system: the engine instantiates it with the document and drives it each frame.
    if (host->Drivers != nullptr)
    {
        host->Drivers->Register<TemplateOverlayDriver>();
        // The primary HUD's driver paints the model portrait its entity carries.
        host->Drivers->Register<TemplateHudDriver>();
        // The primary HUD embeds an emblem component that drives its own subtree — its driver is
        // named on the `<Component>` boundary and instantiated by the engine, one per embed.
        host->Drivers->Register<EmblemDriver>();
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
