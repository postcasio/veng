#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetLoaderRegistry.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/Level.h>
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
