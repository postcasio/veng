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

    /// @brief Construction parameters for EditorHost.
    struct EditorHostInfo
    {
        /// @brief Game module the host dlopen's at startup (the libgame being edited).
        ///
        /// Registers types, the Application factory, and — when an editor module is
        /// also present — editor panels and asset editors.
        Veng::path GameModulePath;

        /// @brief Optional game editor-extension module. nullopt skips it.
        Veng::optional<Veng::path> EditorModulePath;

        /// @brief Source-pack manifest (.vengpack.json). Maps an AssetId to its
        /// per-asset JSON source so asset editors know which file to edit and recook.
        ///
        /// nullopt disables source resolution; asset editors have no source to open.
        Veng::optional<Veng::path> AssetManifestPath;

        /// @brief Engine application parameters.
        Veng::ApplicationInfo App;

        /// @brief Cook-on-demand backend, injected by the editor exe (which links
        /// libveng_cook). Null disables cook-on-demand; RequestCook then reports an error.
        VengEditor::CookBackend Cook;
    };

    /// @brief The editor application: an Application subclass that hosts the module
    /// registries, the EditorRegistry, a panel set in a top-level ImGui dockspace,
    /// and the scene viewport's SceneRenderer.
    class EditorHost : public Veng::Application, public PanelHost
    {
    public:
        /// @brief Constructs and returns an EditorHost from the given parameters.
        static Veng::Unique<EditorHost> Create(const EditorHostInfo& info);
        ~EditorHost() override;

        /// @brief Resolves the asset's registered editor and queues the panel for
        /// adoption into the panel set at the next safe point in the frame.
        void OpenAssetEditor(Veng::AssetType type, Veng::AssetId id) override;

        /// @brief Cooks a source asset on demand through the injected cook backend
        /// and shadow-mounts the result so Load<T>(request.TargetId) resolves it.
        ///
        /// onComplete fires on the main thread with a live MountHandle or an error.
        /// A cook error is also logged via Log::Error. Reports an error when no
        /// backend is configured.
        /// @param request    The source asset and target id to cook.
        /// @param onComplete Continuation called on the main thread with the result.
        void RequestCook(const VengEditor::CookRequest& request,
                         Veng::function<void(Veng::Result<Veng::MountHandle>)> onComplete);

    protected:
        /// @brief Initializes the panel set, shaders, blit pipeline, and source index.
        void OnInitialize() override;
        /// @brief Records per-frame render passes and drives the ImGui dockspace.
        void OnRender() override;
        /// @brief Releases all panels and GPU resources before the context is torn down.
        void OnDispose() override;

    private:
        /// @brief Owned registries, constructed before this Application so the base
        /// can borrow the TypeRegistry by reference.
        struct Registries;
        /// @brief Private constructor; use Create().
        EditorHost(const EditorHostInfo& info, Veng::Unique<Registries> registries,
                   Veng::Unique<Veng::LoadedModule> gameModule,
                   Veng::optional<Veng::LoadedModule> editorModule);

        /// @brief Draws the main menu bar (File / Window menus).
        void DrawMenuBar();

        /// @brief Builds and compiles the present render graph: a fullscreen blit of
        /// the ImGui output into the swapchain. Recompiled on swapchain resize.
        Veng::Unique<Veng::Renderer::CompiledGraph> BuildPresentGraph();

        EditorHostInfo m_Info;

        /// @brief Loaded modules; must outlive every registered closure and reflected
        /// descriptor. Declared before m_Registries so they are destroyed after it
        /// (C++ destroys members in reverse declaration order).
        Veng::Unique<Veng::LoadedModule> m_GameModule;
        /// @brief Optional game editor-extension module.
        Veng::optional<Veng::LoadedModule> m_EditorModule;

        /// @brief Declared after the modules so it is destroyed first; its
        /// ApplicationRegistry holds closures whose code lives in the game module.
        Veng::Unique<Registries> m_Registries;

        /// @brief AssetId to source-file index, parsed once from the manifest.
        /// nullptr when no manifest path is configured.
        Veng::Unique<AssetSourceIndex> m_Sources;

        /// @brief One open panel slot with its Window-menu visibility flag.
        struct PanelSlot
        {
            /// @brief The panel instance.
            Veng::Unique<EditorPanel> Panel;
            /// @brief Whether the panel's window is currently open.
            bool Open = true;
        };
        /// @brief Host-owned panel set: built-ins plus any game-contributed panels.
        Veng::vector<PanelSlot> m_Panels;

        /// @brief Non-owning pointer into m_Panels' viewport slot, used to drive the
        /// scene render before the UI is built each frame.
        SceneViewportPanel* m_Viewport = nullptr;

        /// @brief Non-owning pointer into m_Panels' inspector slot, fed the viewport's
        /// scene and selection each frame before the UI is built.
        InspectorPanel* m_Inspector = nullptr;

        /// @brief Panels opened via OpenAssetEditor since the last frame; adopted into
        /// m_Panels outside panel-iteration so opening from OnImGui is safe.
        Veng::vector<Veng::Unique<EditorPanel>> m_PendingPanels;

        /// @brief Vertex shader for the fullscreen ImGui-to-swapchain blit.
        Veng::AssetHandle<Veng::Shader> m_BlitVS;
        /// @brief Fragment shader for the fullscreen ImGui-to-swapchain blit.
        Veng::AssetHandle<Veng::Shader> m_BlitFS;
        /// @brief Pipeline layout for the blit pass (push constants: texture + sampler handles).
        Veng::Ref<Veng::Renderer::PipelineLayout> m_BlitLayout;
        /// @brief Graphics pipeline for the blit pass.
        Veng::Ref<Veng::Renderer::GraphicsPipeline> m_BlitPipeline;
        /// @brief Sampler used to read the ImGui output texture during the blit.
        Veng::Ref<Veng::Renderer::Sampler> m_Sampler;
        /// @brief Image view over the ImGui layer's output image.
        Veng::Ref<Veng::Renderer::ImageView> m_ImGuiView;
        /// @brief Bindless handle for the ImGui output texture.
        Veng::Renderer::TextureHandle m_ImGuiHandle;
        /// @brief Bindless handle for the blit sampler.
        Veng::Renderer::SamplerHandle m_SamplerHandle;

        /// @brief Compiled present render graph; rebuilt on swapchain resize.
        Veng::Unique<Veng::Renderer::CompiledGraph> m_PresentGraph;
        /// @brief Render graph resource id for the swapchain image.
        Veng::Renderer::ResourceId m_SwapId;
        /// @brief Render graph resource id for the ImGui output image.
        Veng::Renderer::ResourceId m_ImGuiId;
    };
}
