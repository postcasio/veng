#pragma once

#include <Veng/Application.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>
#include <VengEditor/EditorRegistry.h>
#include <VengEditor/PanelHost.h>

namespace Veng
{
    class Shader;

    namespace Renderer
    {
        class GraphicsPipeline;
        class PipelineLayout;
        class Sampler;
        class ImageView;
    }
}

namespace VengEditor
{
    class SceneViewportPanel;
    class InspectorPanel;
    class AssetSourceIndex;

    struct EditorHostInfo
    {
        // The game module the host dlopen's at startup (the libgame the editor
        // edits). It registers its types and Application factory; with an editor
        // module also present, its editor panels and asset editors.
        Veng::path GameModulePath;

        // The optional libgame_editor — the game's editor extensions. nullopt
        // skips it; the game module alone still loads.
        Veng::optional<Veng::path> EditorModulePath;

        // The pack source manifest (the .vengpack.json the cooker reads). The
        // editor maps an AssetId to its per-asset JSON source through it, so an
        // asset editor knows which source file to edit and recook. nullopt
        // disables source resolution — asset editors then have no source to open.
        Veng::optional<Veng::path> AssetManifestPath;

        Veng::ApplicationInfo App;

        // The cook-on-demand backend, supplied by the editor exe (which links
        // libveng_cook). nullopt disables cook-on-demand — RequestCook then
        // reports an error to the callback. libveng_editor itself never links the
        // importer table, so the backend is injected from the exe layer.
        VengEditor::CookBackend Cook;
    };

    // The editor application: an Application subclass owning the host-side module
    // registries (the same pattern as the launcher), the EditorRegistry the
    // module registers into, a panel set drawn into a top-level ImGui dockspace,
    // and the scene viewport's SceneRenderer.
    class EditorHost : public Veng::Application, public PanelHost
    {
    public:
        static Veng::Unique<EditorHost> Create(const EditorHostInfo& info);
        ~EditorHost() override;

        // PanelHost: resolve the asset's editor through the registry and queue it
        // for adoption into the panel set at the next safe point in the frame.
        void OpenAssetEditor(Veng::AssetType type, Veng::AssetId id) override;

        // Cooks a source asset on demand through the injected cook backend (off
        // the render thread) and, on success, shadow-mounts the resulting
        // in-memory archive so Load<T>(request.TargetId) resolves the cooked blob.
        // onComplete fires on the main thread with a live MountHandle (mounted,
        // ready to Load) or an error. A cook error is also logged via Log::Error
        // for the console panel. Calling with no cook backend reports an error.
        void RequestCook(const VengEditor::CookRequest& request,
                         Veng::function<void(Veng::Result<Veng::MountHandle>)> onComplete);

    protected:
        void OnInitialize() override;
        void OnRender() override;
        void OnDispose() override;

    private:
        // Owned registries, constructed before this app so the base Application
        // may borrow the TypeRegistry by reference. Their definitions live in
        // libveng (ApplicationRegistry) and libveng_editor (EditorRegistry).
        struct Registries;
        EditorHost(const EditorHostInfo& info, Veng::Unique<Registries> registries,
                   Veng::Unique<Veng::LoadedModule> gameModule,
                   Veng::optional<Veng::LoadedModule> editorModule);

        void DrawMenuBar();

        // Build + compile the present graph: a fullscreen blit of the ImGui
        // output into the swapchain image. Re-compiled on swapchain resize.
        Veng::Unique<Veng::Renderer::CompiledGraph> BuildPresentGraph();

        EditorHostInfo m_Info;

        // The loaded modules must outlive every registered closure and reflected
        // descriptor (code/data in the module images). Declared before m_Registries
        // so they are destroyed AFTER it (C++ destroys members in reverse
        // declaration order).
        Veng::Unique<Veng::LoadedModule> m_GameModule;
        Veng::optional<Veng::LoadedModule> m_EditorModule;

        // Declared after the modules so it is destroyed first; its ApplicationRegistry
        // holds a function<> whose closure code lives in the game module.
        Veng::Unique<Registries> m_Registries;

        // The shared AssetId -> source manifest index, parsed once and referenced
        // by the inspector's asset picker and the asset-editor factories. nullopt
        // when no manifest path is configured.
        Veng::Unique<AssetSourceIndex> m_Sources;

        // The host-owned panel set: built-ins plus any game-contributed panels,
        // each with an open/close flag the Window menu toggles.
        struct PanelSlot
        {
            Veng::Unique<EditorPanel> Panel;
            bool Open = true;
        };
        Veng::vector<PanelSlot> m_Panels;

        // Non-owning: points into m_Panels' viewport slot, used to drive the scene
        // render before the UI is built each frame.
        SceneViewportPanel* m_Viewport = nullptr;

        // Non-owning: points into m_Panels' inspector slot, fed the viewport's
        // scene and the current selection each frame before the UI is built.
        InspectorPanel* m_Inspector = nullptr;

        // Panels opened via OpenAssetEditor since the last frame, adopted into
        // m_Panels at a point outside the panel-iteration so opening from inside a
        // panel's OnImGui is safe.
        Veng::vector<Veng::Unique<EditorPanel>> m_PendingPanels;

        // The present pipeline: a fullscreen blit of the ImGui output into the
        // swapchain, addressed through the bindless set 0.
        Veng::AssetHandle<Veng::Shader> m_BlitVS;
        Veng::AssetHandle<Veng::Shader> m_BlitFS;
        Veng::Ref<Veng::Renderer::PipelineLayout> m_BlitLayout;
        Veng::Ref<Veng::Renderer::GraphicsPipeline> m_BlitPipeline;
        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        Veng::Ref<Veng::Renderer::ImageView> m_ImGuiView;
        Veng::Renderer::TextureHandle m_ImGuiHandle;
        Veng::Renderer::SamplerHandle m_SamplerHandle;

        Veng::Unique<Veng::Renderer::CompiledGraph> m_PresentGraph;
        Veng::Renderer::ResourceId m_SwapId;
        Veng::Renderer::ResourceId m_ImGuiId;
    };
}
