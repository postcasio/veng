#pragma once

#include <Veng/Application.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Module/ModuleLoader.h>
#include <Veng/Project/ProjectSettings.h>
#include <Veng/Reflection/TypeRegistry.h>

#include <VengEditor/CookRequest.h>
#include <VengEditor/EditorPanel.h>
#include <VengEditor/EditorRegistry.h>
#include <VengEditor/PanelHost.h>

namespace VengEditor
{
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

        /// @brief Authoring project file (project.veng) the editor opens.
        ///
        /// The editor reads the packs the project owns to map an AssetId to its per-asset JSON
        /// source (so asset editors know which file to edit and recook) and loads the build
        /// configurations. nullopt disables source resolution; asset editors have no source to open.
        Veng::optional<Veng::path> ProjectPath;

        /// @brief Engine application parameters.
        Veng::ApplicationInfo App;

        /// @brief Cook-on-demand backend, injected by the editor exe (which links
        /// libveng_cook). Null disables cook-on-demand; RequestCook then reports an error.
        VengEditor::CookBackend Cook;
    };

    /// @brief The editor application: an Application subclass that hosts the module
    /// registries, the EditorRegistry, and a panel set in a top-level ImGui dockspace.
    ///
    /// Registers no Presented viewport — the dockspace is opaque ImGui, so the engine's
    /// managed gather/composite tail runs with zero placements (a cleared assembly target,
    /// ImGui over it). Render-owning panels hold their own registered Offscreen viewports.
    class EditorHost : public Veng::Application, public PanelHost
    {
    public:
        /// @brief Constructs and returns an EditorHost from the given parameters.
        static Veng::Unique<EditorHost> Create(const EditorHostInfo& info);
        ~EditorHost() override;

        /// @brief Resolves the asset's registered editor and queues the panel for
        /// adoption into the panel set at the next safe point in the frame.
        void OpenAssetEditor(Veng::AssetType type, Veng::AssetId id) override;

        /// @brief Returns whether the editor registry has a factory for the asset type.
        [[nodiscard]] bool HasAssetEditor(Veng::AssetType type) const override;

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

        /// @brief Returns the project's active build configuration, or nullptr when none resolves.
        ///
        /// Looks the ActiveConfiguration name up in the configuration list. Null when the project
        /// has no configurations or the active name names none — the zero-config state. Used by the
        /// texture editor for its resolved-format read-out and by the cook to resolve roles.
        [[nodiscard]] const Veng::BuildConfiguration* GetActiveConfiguration() const;

        /// @brief Returns the configuration the editor cooks its live preview against.
        ///
        /// Host-safe by default, independent of the selected ship configuration: an uncompressed
        /// profile every GPU can sample, so the editor never hands the device an unsamplable blob.
        /// When the author opts into "preview as ship config" through SetPreviewShipConfig and the
        /// named configuration is previewable on this GPU, that configuration is returned instead.
        /// Building any configuration stays unrestricted — this clamps only the editor's own cook.
        [[nodiscard]] Veng::BuildConfiguration GetPreviewConfiguration();

        /// @brief Returns the name of the ship configuration preview is opted into, or nullopt.
        ///
        /// nullopt is the default host-safe preview. A non-null name is honored by
        /// GetPreviewConfiguration only while that configuration is previewable on this GPU.
        [[nodiscard]] const Veng::optional<Veng::string>& GetPreviewShipConfig() const;

        /// @brief Opts live preview into a named ship configuration, or back to host-safe.
        ///
        /// Passing a configuration name makes GetPreviewConfiguration resolve through it (WYSIWYG
        /// of that target's real codec artifacts) as long as it stays host-previewable; nullopt
        /// reverts to the host-safe default. The caller is expected to offer only previewable
        /// configurations, but the getter re-validates so a config that becomes incompatible falls
        /// back to host-safe rather than failing.
        /// @param name  The ship configuration to preview through, or nullopt for host-safe.
        void SetPreviewShipConfig(Veng::optional<Veng::string> name);

    protected:
        /// @brief Initializes the panel set and source index.
        void OnInitialize() override;
        /// @brief Builds the ImGui dockspace, menu bar, and per-panel windows for the frame.
        ///
        /// Records no scene render or composite of its own: the engine drive-list renders the
        /// panels' Offscreen viewports before this runs, and the managed gather/composite tail
        /// runs after ImGuiLayer::Render with zero Presented placements.
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

        /// @brief The host-owned project settings: the build-configuration list and the active
        /// one. Loaded from project.veng beside the manifest at startup, or left empty.
        Veng::ProjectSettings m_ProjectSettings;

        /// @brief Absolute path project.veng saves to (its directory holds the *.buildcfg files).
        /// Empty when no manifest path is configured, which disables saving project settings.
        Veng::path m_ProjectFile;

        /// @brief Ship configuration live preview is opted into, or nullopt for host-safe.
        ///
        /// nullopt (the default) previews against the host-safe uncompressed profile; a name
        /// previews against that ship configuration's real codec formats while it stays
        /// host-previewable. Drives GetPreviewConfiguration.
        Veng::optional<Veng::string> m_PreviewShipConfig;

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

        /// @brief Panels opened via OpenAssetEditor since the last frame; adopted into
        /// m_Panels outside panel-iteration so opening from OnUI is safe.
        Veng::vector<Veng::Unique<EditorPanel>> m_PendingPanels;
    };
}
