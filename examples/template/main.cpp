#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Scene.h>

#include <fmt/format.h>

#include <algorithm>

using namespace Veng;

// The HUD's view-model: the game-owned data its bindings read. `{Caption}` and `{Level}` in the
// markup resolve their field paths against this reflected struct through the TypeRegistry.
struct TemplateHud
{
    string Caption = "warming up";
    f32 Level = 0.0f;
};

VE_REFLECT(::TemplateHud, 0x0D0C072CE1127CF4ULL)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();

// The smallest veng game that also authors a HUD: the bare managed-world app (a rotating cube driven
// entirely by cooked data) grows a minimal Application subclass whose only job is the data binding
// the engine cannot do from data alone. The HUD document is authored on an entity in the world
// prefab as a GuiOverlay, so the Viewport owns its load / instantiate / attach; the subclass finds
// that component and hands it the view-model, then feeds the bound fields each frame.
class TemplateApp final : public Application
{
public:
    using Application::Application;

private:
    // The world is loaded here; find the prefab-authored GuiOverlay and bind it the view-model. The
    // bind is deferred — the overlay applies it when the Viewport instantiates the document — so this
    // runs before the first render with no ordering hole.
    void OnWorldLoaded(Scene& world, ResidencyBatch&) override
    {
        m_Context.SetData(m_Model);
        for (auto [entity, overlay] : world.View<GuiOverlay>())
        {
            overlay.SetContext(&m_Context);
        }
    }

    // Feed the bound fields each frame and mark the context changed; the Viewport's per-frame overlay
    // drive re-resolves the bindings and composites the updated HUD, so the game writes no layout or
    // attach code. The caption reports the running frame rate and the bar tracks it.
    void OnUpdate(const f32 delta) override
    {
        m_Model.Caption = fmt::format("{:.0f} fps", delta > 0.0f ? 1.0f / delta : 0.0f);
        m_Model.Level = std::clamp(delta > 0.0f ? (1.0f / delta) / 120.0f : 0.0f, 0.0f, 1.0f);
        m_Context.Invalidate();
    }

    TemplateHud m_Model;
    Gui::BindingContext m_Context;
};

extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<TemplateHud>();
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
                    // the resolved camera each frame. The subclass adds only the HUD document.
                    .ManagedViewport = ManagedViewportInfo{},
                    .World = GameWorldInfo{.Project = "project.vengproj"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()
