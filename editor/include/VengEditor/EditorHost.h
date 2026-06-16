#pragma once

#include <Veng/Application.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>

#include <VengEditor/EditorPanel.h>
#include <VengEditor/EditorRegistry.h>

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

    struct EditorHostInfo
    {
        // The game module the host dlopen's at startup (the libgame the editor
        // edits). It registers its types and Application factory; with an editor
        // module also present, its editor panels and asset editors.
        Veng::path GameModulePath;

        // The optional libgame_editor — the game's editor extensions. nullopt
        // skips it; the game module alone still loads.
        Veng::optional<Veng::path> EditorModulePath;

        Veng::ApplicationInfo App;
    };

    // The editor application: an Application subclass owning the host-side module
    // registries (the same pattern as the launcher), the EditorRegistry the
    // module registers into, a panel set drawn into a top-level ImGui dockspace,
    // and the scene viewport's SceneRenderer.
    class EditorHost : public Veng::Application
    {
    public:
        static Veng::Unique<EditorHost> Create(const EditorHostInfo& info);
        ~EditorHost() override;

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

        Veng::Unique<Registries> m_Registries;

        // The loaded modules outlive every registered closure and reflected
        // descriptor (which are code/data in the module images), so they are
        // released last in OnDispose-then-destructor order.
        Veng::Unique<Veng::LoadedModule> m_GameModule;
        Veng::optional<Veng::LoadedModule> m_EditorModule;

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
