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
    class AssetEditorPanel;
    class AssetSourceIndex;
    class CommandStack;
    class StatusTracker;

    /// @brief Construction parameters for EditorHost.
    struct EditorHostInfo
    {
        /// @brief Authoring project file (project.veng) the editor opens — its entrypoint.
        ///
        /// The editor reads the module(s) it dlopens (ProjectSettings::Module / EditorModule), the
        /// packs the project owns (mapping an AssetId to its per-asset JSON source so asset editors
        /// know which file to edit and recook), the build configurations, and the startup level
        /// from it. nullopt leaves the editor with no project: no module loads and asset editors
        /// have no source to open.
        Veng::optional<Veng::path> ProjectPath;

        /// @brief Explicit override for the project's build-output dir — the cooked packs and the
        /// module shared libraries the project names.
        ///
        /// nullopt is the normal case: Create discovers the build dir from the .veng/build.json
        /// sidecar the build wrote beside the project, so launching with only a project works. A
        /// non-null value overrides that discovery (CI, an unusual layout). With neither, Create
        /// falls back to ExecutableDirectory() — the relocatable ship layout, packs + module beside
        /// the editor. The editor's own icon pack always sits beside the editor exe, not here.
        Veng::optional<Veng::path> BuildDir;

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
        EditorHost(const EditorHostInfo& info, Veng::ProjectSettings settings, Veng::path buildDir,
                   Veng::path corePackManifest, Veng::Unique<Registries> registries,
                   Veng::Unique<Veng::LoadedModule> gameModule,
                   Veng::optional<Veng::LoadedModule> editorModule);

        /// @brief Draws the main menu bar (File / Edit / Window menus).
        void DrawMenuBar();

        /// @brief Draws the bottom status bar showing in-flight long-running tasks.
        ///
        /// Reserves a strip at the bottom of the main viewport (so the dockspace fits above
        /// it) and, while any task tracked by m_Status is running, draws a description and a
        /// progress bar — the task's own label with an indeterminate sweep for a single task,
        /// or "N tasks running" with the wave's completed/total fill for several. Idle leaves
        /// the strip empty.
        void DrawStatusBar();

        /// @brief Returns the focused AssetEditorPanel, or null when none holds keyboard focus.
        ///
        /// Resolves the editor whose window or a docked child holds focus — the seam the Edit-menu
        /// / undo-redo shortcuts and the File→Save / Ctrl+S action dispatch to, so two open
        /// documents are independent.
        [[nodiscard]] AssetEditorPanel* FocusedAssetEditor();

        /// @brief Returns the focused document's undo/redo stack, or null when none is focused.
        ///
        /// Resolves the focused AssetEditorPanel (an editor whose window or a docked child holds
        /// keyboard focus) and returns its CommandStack — the seam the Edit menu and the undo/redo
        /// shortcuts dispatch to, so two open documents undo independently.
        [[nodiscard]] CommandStack* FocusedCommandStack();

        /// @brief Lays out the host dockspace the first frame no imgui.ini layout exists.
        ///
        /// Docks the asset browser, console and project settings as tabs in a bottom strip and
        /// flags the remaining upper region as the central node, into which asset editors dock
        /// through the pending-adoption path. A layout restored from imgui.ini is left untouched,
        /// so user docking survives a restart.
        /// @param dockspaceId  The host dockspace node to populate.
        /// @return The id of the central (main dock area) node, for docking documents into it.
        Veng::u32 BuildDefaultHostLayout(Veng::u32 dockspaceId);

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

        /// @brief Tracks in-flight long-running tasks (cooks) the status bar reports.
        Veng::Unique<StatusTracker> m_Status;

        /// @brief The host-owned project settings: the build-configuration list and the active
        /// one. Loaded from project.veng beside the manifest at startup, or left empty.
        Veng::ProjectSettings m_ProjectSettings;

        /// @brief Absolute path project.veng saves to (its directory holds the *.buildcfg files).
        /// Empty when no manifest path is configured, which disables saving project settings.
        Veng::path m_ProjectFile;

        /// @brief The resolved build-output dir (cooked packs + module libraries), fixed in Create.
        ///
        /// The override, the .veng/build.json sidecar discovery, or ExecutableDirectory(), resolved
        /// once so every pack mount and the level-cook module path key off one value.
        Veng::path m_BuildDir;

        /// @brief The engine core pack's source manifest, discovered from the .veng/build.json
        /// sidecar; passed as a cook-on-demand reference so a recooked asset resolves core-pack ids.
        ///
        /// Empty in the relocatable ship layout (no sidecar), where the editor edits no source.
        Veng::path m_CorePackManifest;

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
            /// @brief A closeable document (an opened asset editor), destroyed when its window
            /// closes; a false value is a persistent tool panel that only hides.
            ///
            /// A document owns Offscreen viewport(s) the engine drive-list renders every frame,
            /// so closing it must release them (and its dock windows) rather than leave it hidden
            /// but live — a hidden editor would keep rendering its viewport and litter the docking.
            bool Document = false;
        };
        /// @brief Host-owned panel set: built-ins plus any game-contributed panels.
        Veng::vector<PanelSlot> m_Panels;

        /// @brief Panels opened via OpenAssetEditor since the last frame; adopted into
        /// m_Panels outside panel-iteration so opening from OnUI is safe.
        Veng::vector<Veng::Unique<EditorPanel>> m_PendingPanels;

        /// @brief Whether the one-time default host dock layout has been attempted.
        bool m_HostLayoutBuilt = false;
    };
}
