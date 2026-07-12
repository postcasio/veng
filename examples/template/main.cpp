#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Input.h>
#include <Veng/LevelOverlay.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

#include <fmt/format.h>

#include <algorithm>
#include <optional>

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

// Drives the overlay level's own HUD: the one system named in the overlay level's `systems` beside
// the builtin input systems. On its first tick it binds the interactive HUD a view-model plus a
// "Dismiss" handler, then each frame mirrors the button's press into the OverlayControl the opener
// drains. It reads its snapshot and drives its HUD entirely within the overlay scene — the "modal
// live scene" runs its own simulation, its HUD driven by its own viewport, taking input on its own
// seat.
class TemplateOverlaySystem final : public SceneSystem
{
public:
    // Presentation binding derived from finalized Sim state, so it runs in the View phase.
    [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

    void OnUpdate(Scene& scene, f32, const SystemContext&) override
    {
        if (!m_Bound)
        {
            // Seed the model from the populate-hook snapshot, then bind it plus the dismiss handler
            // to every GuiOverlay in the overlay scene. The bind is deferred inside the overlay until
            // its Viewport instantiates the document, so binding here (before the first render) has
            // no ordering hole.
            if (const OverlaySnapshot* snapshot = scene.TryGetFirst<OverlaySnapshot>())
            {
                m_Model = *snapshot;
            }
            m_Context.SetData(m_Model);
            m_Context.SetHandler("Dismiss", [this](Gui::Element&) { m_DismissRequested = true; });
            for (auto [entity, overlay] : scene.View<GuiOverlay>())
            {
                overlay.SetContext(&m_Context);
            }
            m_Bound = true;
        }

        // Publish the button's press into the drained channel; the opener reads it and closes.
        if (auto* control = scene.TryGetFirst<OverlayControl>())
        {
            control->Requested = m_DismissRequested;
        }
    }

private:
    OverlaySnapshot m_Model;
    Gui::BindingContext m_Context;
    bool m_DismissRequested = false;
    bool m_Bound = false;
};

VE_SYSTEM(TemplateOverlaySystem, 0x2BCEBD75A23A39F1ULL, "Template Overlay");

// The cooked overlay level the Tab key opens as a secondary, simulated overlay. Its own prefab
// authors an input seat, a spinning cube, and an interactive GuiOverlay HUD; its `systems` name the
// builtin input systems plus TemplateOverlaySystem.
constexpr AssetId OverlayLevelId{0x88B360A2DD16632EULL};

// The smallest veng game that also authors a HUD and opens a live sub-scene: the bare managed-world
// app (a rotating cube driven entirely by cooked data) grows a minimal Application subclass. Its
// jobs are the primary HUD's data binding (the one thing the engine cannot do from data alone) and
// the lifecycle of a secondary overlay level opened on a key. The primary HUD is authored on an
// entity in the world prefab as a GuiOverlay, so the Viewport owns its load / instantiate / attach.
class TemplateApp final : public Application
{
public:
    using Application::Application;

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

        // Hold the overlay level asset resident so opening it is a spawn, not a load. Open still
        // waits on the spawn's residency (WaitForResidency), accepting the first-open hitch.
        if (const auto level = GetAssetManager().LoadSync<Level>(OverlayLevelId))
        {
            m_OverlayLevel = *level;
        }
    }

    // Feed the primary HUD's bound fields, toggle the overlay on Tab, and — while it is open — tick it
    // and drain its dismiss channel. The Viewport's per-frame overlay drive re-resolves the primary
    // bindings and composites the HUD, so the game writes no layout or attach code.
    void OnUpdate(const f32 delta) override
    {
        m_Model.Caption = fmt::format("{:.0f} fps", delta > 0.0f ? 1.0f / delta : 0.0f);
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
            // The engine ticks the overlay's simulation (it registers as one on Open); Update only
            // re-applies the region and pushes the view each frame.
            m_Overlay->Update(delta);

            // Dismiss on the HUD's button: the overlay system published the click into OverlayControl,
            // which the opener owns and drains. (Tab, above, is the other dismissal.)
            const OverlayControl* const control =
                m_Overlay->GetScene().TryGetFirst<OverlayControl>();
            if (control != nullptr && control->Requested)
            {
                m_Overlay.reset();
            }
        }
    }

    void OnDispose() override { m_Overlay.reset(); }

    // Opens the overlay level over the running frame: a fresh scene simulated concurrently, its own
    // seat taking input while the primary's is suspended, and — the opt-in PausePrimarySim knob —
    // the primary's simulation frozen for the modal's lifetime. The populate hook copies a snapshot
    // of the primary HUD's state into the overlay scene before it starts.
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
                .PausePrimarySim = true,
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

    // The overlay level asset, held resident from OnWorldLoaded, and the live handle while it is open.
    AssetHandle<Level> m_OverlayLevel;
    std::optional<LevelOverlay> m_Overlay;
};

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<TemplateHud>();
    host->Types.Register<OverlaySnapshot>();
    host->Types.Register<OverlayControl>();
    host->Systems.Register<TemplateOverlaySystem>();
    host->App.RegisterApplication(
        [](TypeRegistry& types, SystemRegistry& systems)
        {
            return Unique<Application>(new TemplateApp(
                ApplicationInfo{
                    .Name = "Template",
                    .WindowInfo =
                        {
                            .Extent = {1280, 720},
                            .Title = "veng — Template",
                        },
                    // The engine owns the primary viewport (its SceneRenderer + the gather +
                    // composite tail) and drives the managed world: it reads the cooked project,
                    // mounts its packs, loads the startup level, ticks the simulation, and pushes
                    // the resolved camera each frame. The subclass adds the HUD binding and the
                    // secondary overlay level.
                    .ManagedViewport = ManagedViewportInfo{},
                    .World = GameWorldInfo{.Project = "project.vengproj"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
