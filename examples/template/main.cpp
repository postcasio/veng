#include <Veng/Application.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>

#include <algorithm>

using namespace Veng;

namespace
{
    // The cooked UI document authored under assets/ui/hud.vui.xml — a status panel bound to the
    // view-model below. Minted with `vengc generate-id --reference`.
    constexpr AssetId HudDocumentId{0x5F30E0E78A88D61DULL};
}

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

// The smallest veng game that also authors a HUD: the bare managed-world app (a rotating cube
// driven entirely by cooked data) grows a minimal Application subclass that instantiates a cooked
// UIDocument, binds it a view-model, and attaches it to the managed viewport — the one wiring the
// engine cannot do from data alone. The document then draws and updates itself; the subclass only
// feeds its bound fields each frame.
class TemplateApp final : public Application
{
public:
    using Application::Application;

private:
    // The world is loaded here; load the cooked HUD, instantiate a live tree (resolving its font
    // through the asset manager), bind the view-model, and attach it over the managed viewport.
    void OnWorldLoaded(Scene&, ResidencyBatch&) override
    {
        AssetManager& assets = GetAssetManager();
        const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
            assets.LoadSync<Gui::UIDocument>(HudDocumentId);
        if (!recipe)
        {
            return;
        }

        m_Hud = Gui::Document::Instantiate(*recipe->Get(), assets);
        m_Context.SetData(m_Model);
        m_Hud->BindContext(&m_Context, &GetTypeRegistry());
        GetPrimaryViewport()->AttachDocument(*m_Hud);
    }

    // Feed the bound fields each frame and re-resolve the document's bindings; the engine's per-frame
    // drive then lays out and composites the updated HUD. The caption reports the running frame rate
    // and the bar tracks it.
    void OnUpdate(const f32 delta) override
    {
        if (!m_Hud)
        {
            return;
        }
        m_Model.Caption = fmt::format("{:.0f} fps", delta > 0.0f ? 1.0f / delta : 0.0f);
        m_Model.Level = std::clamp(delta > 0.0f ? (1.0f / delta) / 120.0f : 0.0f, 0.0f, 1.0f);
        m_Context.Invalidate();
        m_Hud->UpdateBindings();
    }

    // Release the document before the context tears down; dropping the Unique self-detaches it.
    void OnDispose() override { m_Hud.reset(); }

    TemplateHud m_Model;
    Gui::BindingContext m_Context;
    Unique<Gui::Document> m_Hud;
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
