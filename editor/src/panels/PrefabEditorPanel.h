#pragma once

#include <Veng/Path.h>

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Result.h>
#include <Veng/Renderer/ViewportId.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/WorldRunner.h>

#include <VengEditor/AssetEditorPanel.h>
#include "CommandStack.h"
#include "panels/PrefabEditContext.h"

namespace Veng
{
    class Application;
    class AssetManager;
    class EditorRegistry;
    class ImGuiLayer;
    class Input;
    class InputRouter;
    class Prefab;
    class Scene;
    class SystemRegistry;
    class TypeRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;
    class SceneViewportPanel;
    struct PreviewLook;

    /// @brief Asset editor for a prefab: a private dockspace hosting a scene viewport,
    /// an entity-hierarchy explorer, and a reflection inspector over one spawned Scene.
    ///
    /// On open the prefab is loaded and spawned into a fresh Scene the document owns, and shown
    /// under the project's preview look (PreviewLook): its render block and field of view, and
    /// its environment as the sky when the prefab carries none. A default directional light is
    /// added when the prefab would otherwise be unlit. The explorer drives selection, the inspector edits the selected entity's
    /// components, and the viewport renders the live scene — all sharing one
    /// PrefabEditContext.
    class PrefabEditorPanel : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the prefab at @p id.
        /// @param id        The prefab asset to edit.
        /// @param app       Application the viewport registers its Offscreen viewport into.
        /// @param assets    Asset manager the prefab and its dependencies load through.
        /// @param imgui     ImGui layer the viewport registers its render target with.
        /// @param types     Type registry the spawned Scene pools components against.
        /// @param editors   Editor registry for inspector field-widget overrides.
        /// @param sources   Manifest source index for the inspector's asset pickers.
        /// @param input     Frame-coherent input service the viewport camera reads.
        /// @param router    Input router whose gameplay focus captures the mouse during Play.
        /// @param systems   System registry the play session instantiates its systems from.
        /// @param look      The project's preview look; must outlive the panel.
        PrefabEditorPanel(Veng::AssetId id, Veng::Application& app, Veng::AssetManager& assets,
                          Veng::ImGuiLayer& imgui, Veng::TypeRegistry& types,
                          Veng::EditorRegistry& editors, const AssetSourceIndex& sources,
                          Veng::Input& input, Veng::InputRouter& router,
                          Veng::SystemRegistry& systems, const PreviewLook& look);
        ~PrefabEditorPanel() override;

        /// @brief The document window title, carrying an unsaved-changes marker when dirty.
        ///
        /// Recomputed each Draw: a leading "*" when the command stack has unsaved edits. A stable
        /// "##" id suffix keeps the ImGui dock identity constant, so the marker never re-docks the
        /// window.
        [[nodiscard]] Veng::string_view GetTitle() const override;

        /// @brief Writes the document's edited Scene back to its .prefab.json source, atomically.
        ///
        /// Serializes the live scene through the reflection-driven writer (preserving unknown keys,
        /// keyed by stable entity id) to a temp sibling then renames it over the source, and clears
        /// the command stack's dirty flag on success. A no-op (returns an error) when the document
        /// has no source path. Does not recook — the cook-on-demand loop re-reads the changed
        /// source on its own debounce. The level editor inherits this, so its entity edits save to
        /// the referenced world .prefab.json (its level-config save is separate).
        /// @return Empty on success; an error string on a missing source or an I/O failure.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Clones the edit scene and opens it as a world of the host's runner, playing.
        ///
        /// The clone is seeded (SeedPlayScene), then opened through WorldRunner::OpenWorld over the
        /// scene with the set GetPlaySystems() names — every registered system for a prefab document,
        /// the level's ordered set for a level document — and started, so the runner ticks it as it
        /// ticks any world. The world drains its requests under a Sandboxed policy whose exit stops
        /// play, and the document viewport is registered as its presentation (through the clone's
        /// presentation seat, pushing its own camera). Repoints the shared context at the world's
        /// scene, clears the selection (its handles point into the edit scene) and captures the
        /// cursor. A no-op while already playing.
        void Play();

        /// @brief Stops the play session: closes its world and restores the edit scene.
        ///
        /// Releases the cursor and the panel's pause, unregisters the viewport's presentation, and
        /// closes the world (each system's OnStop runs, unless the world already closed). Repoints the
        /// shared context back at the edit scene, clears the selection, and returns to Editing. A
        /// no-op while not playing.
        void Stop();

        /// @brief Holds a pause on the play world and frees the cursor. No-op unless playing and
        ///        not already holding one.
        void Pause();

        /// @brief Drops the panel's own pause on the play world and recaptures the cursor.
        ///
        /// A pause the running game requested (PauseRequest) is the game's to release, so this
        /// releases only the hold Pause took. No-op unless the panel holds one.
        void Resume();

        /// @brief Draws the document toolbar above the dockspace: play transport, gizmo mode, entity count.
        ///
        /// Also follows the play session first (UpdatePlaySession), so the document draws over the
        /// world's live scene and returns to editing once that world has closed.
        void OnUI() override;

        /// @brief Returns this document's undo/redo stack — the seam the host dispatches shortcuts to.
        [[nodiscard]] CommandStack* GetCommandStack() override { return &m_Commands; }

        /// @brief Returns true when the edited scene has unsaved command-stack edits.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Commands.IsDirty(); }

        /// @brief Returns the document's live scene — the play world's while playing, else the edit
        ///        scene — the world tools' focused scene.
        ///
        /// Resolved through the runner while playing, so a play world closed since the last frame
        /// yields null rather than its destroyed scene.
        [[nodiscard]] Veng::Scene* GetDocumentScene() override;

        /// @brief Returns the scene viewport's Offscreen viewport — the screenshot seam's target.
        [[nodiscard]] Veng::Renderer::Viewport* GetDocumentViewport() override;

    protected:
        /// @brief Draws the shared document toolbar: the play transport and the gizmo-mode segment.
        ///
        /// Drawn above the dockspace by OnUI (and by the level editor's OnUI override), so the
        /// play/stop/pause transport and the Move/Rotate/Scale selector sit at document level
        /// rather than on the viewport overlay. The gizmo segment drives the shared document mode
        /// (PrefabEditContext::Gizmo) every viewport's gizmo reads.
        void DrawDocumentToolbar();

        /// @brief Follows the play session this frame; a no-op while editing.
        ///
        /// The runner ticks the play world (before the editor's UI runs), so this only follows it:
        /// a world that no longer resolves — its sandboxed exit, or a system closing it — returns the
        /// document to editing; otherwise it repoints the context at the world's live scene, applies
        /// the Shift+Esc cursor release, keeps the pointer association in step with the capture, and
        /// reads the world's interpolation fraction. The base OnUI calls this; a subclass overriding
        /// OnUI (the level editor) must call it too, before drawing anything over the context.
        void UpdatePlaySession();

        /// @brief Constructs the document over a world prefab id, deferring child wiring to a subclass.
        ///
        /// Builds the edit Scene from @p worldPrefab but adds no child panels — a subclass
        /// (the level editor) calls AddSceneEditingChildren and adds its own panels, then
        /// arranges them in its own BuildDefaultLayout. Used to compose the scene-editing
        /// surface into a richer editor without reimplementing it.
        /// @param worldPrefab The prefab spawned into the edit scene.
        /// @param title       The document window title.
        /// @param app         Application the viewport registers its Offscreen viewport into.
        /// @param assets       Asset manager the prefab and its dependencies load through.
        /// @param imgui        ImGui layer the viewport registers its render target with.
        /// @param types        Type registry the spawned Scene pools components against.
        /// @param editors      Editor registry for inspector field-widget overrides.
        /// @param sources      Manifest source index for the inspector's asset pickers.
        /// @param input        Frame-coherent input service the viewport camera reads.
        /// @param router       Input router whose gameplay focus captures the mouse during Play.
        /// @param systems      System registry the play session instantiates its systems from.
        /// @param look         The preview look to show the scene under, or nullptr to show it
        ///                     under the viewport's defaults (a level brings its own render block
        ///                     and sky); must outlive the panel.
        PrefabEditorPanel(Veng::AssetId worldPrefab, Veng::string title, Veng::Application& app,
                          Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                          Veng::TypeRegistry& types, Veng::EditorRegistry& editors,
                          const AssetSourceIndex& sources, Veng::Input& input,
                          Veng::InputRouter& router, Veng::SystemRegistry& systems,
                          const PreviewLook* look);

        /// @brief Splits the dockspace into explorer (left), viewport (center), inspector (right).
        void BuildDefaultLayout(Veng::u32 dockspaceId) override;

        /// @brief The ordered system set Play runs, or nullptr to run every registered system.
        ///
        /// The prefab document runs every registered system (a debugging convenience); a level
        /// document overrides this to return its authored ordered set.
        /// @return The level's ordered SystemId set, or nullptr for the "all registered" set.
        [[nodiscard]] virtual const Veng::vector<Veng::SystemId>* GetPlaySystems() const
        {
            return nullptr;
        }

        /// @brief Seeds the play clone with any document-scoped entities, before the simulation Starts.
        ///
        /// Called by Play after cloning the edit scene and before the simulation's Start, so the
        /// clone reaches the same initialized state the runtime does. The base is a no-op (a bare
        /// prefab has no settings entity); the level editor overrides it to seed the game-mode
        /// config its spawn rule reads.
        /// @param scene The play clone the systems run over.
        virtual void SeedPlayScene(Veng::Scene& /*scene*/) {}

        /// @brief Adds the viewport / explorer / inspector children over the shared edit context.
        ///
        /// Called by a subclass after the base constructor has built the scene, so the level
        /// editor composes the same scene-editing surface a standalone prefab editor uses.
        /// @param app      Application the viewport registers its Offscreen viewport into.
        /// @param imgui    ImGui layer the viewport registers its render target with.
        /// @param editors  Editor registry for inspector field-widget overrides.
        /// @param sources  Manifest source index for the inspector's asset pickers.
        void AddSceneEditingChildren(Veng::Application& app, Veng::ImGuiLayer& imgui,
                                     Veng::EditorRegistry& editors,
                                     const AssetSourceIndex& sources);

        /// @brief The shared edit context the scene-editing children and a subclass operate over.
        PrefabEditContext m_Context;

        /// @brief This document's undo/redo history; constructed over m_Context, after it.
        ///
        /// The explorer / inspector / gizmo route their mutations through it; the host dispatches
        /// the Edit menu and the undo/redo shortcuts here when this document is focused.
        CommandStack m_Commands{m_Context};

        Veng::usize m_ExplorerChild = 0;
        Veng::usize m_ViewportChild = 0;
        Veng::usize m_InspectorChild = 0;

        /// @brief The viewport child instance, for a subclass to drive renderer-facing state (e.g. level render settings).
        SceneViewportPanel* m_Viewport = nullptr;

        /// @brief The viewport SyncPlayPointer associated with the cursor seat, or invalid for none.
        ///
        /// Kept by id so the association clears even after the viewport is gone.
        Veng::Renderer::ViewportId m_PlayPointerViewport;

        /// @brief The human label part of the title (e.g. "Prefab 0x42"), before the marker/id.
        ///
        /// A subclass (the level editor) sets it to its own label; GetTitle() composes the unsaved
        /// marker and stable id suffix around it.
        Veng::string m_BaseTitle;

        /// @brief The .prefab.json the document saves its entity edits back to, or empty when none.
        ///
        /// Resolved from the manifest source index for a standalone prefab document; the level
        /// editor sets it to the referenced world prefab's source (distinct from its own
        /// .level.json config source). Empty disables Save.
        Veng::path m_PrefabSource;

    private:
        /// @brief Loads and spawns the prefab, lighting it when no preview look will.
        void BuildScene();

        /// @brief Pushes gameplay input focus (capturing the cursor) if not already held.
        void CaptureForPlay();
        /// @brief Pops gameplay input focus (releasing the cursor) if currently held.
        void ReleaseFromPlay();
        /// @brief Associates the viewport with the cursor seat exactly while this document's play
        ///        session holds the cursor capture, so the captured pointer routes to the play scene.
        void SyncPlayPointer();

        Veng::AssetId m_Id;

        /// @brief The preview look the scene is shown under, or nullptr for the viewport defaults.
        const PreviewLook* m_Look = nullptr;

        /// @brief Stable "##doc<id>" suffix keeping the ImGui dock identity constant across the marker.
        Veng::string m_TitleId;
        /// @brief GetTitle()'s recomputed buffer: the marker + label + stable id suffix.
        mutable Veng::string m_DisplayTitle;

        /// @brief The host whose WorldRunner runs the play session, and whose managed set presents it.
        Veng::Application& m_App;
        Veng::AssetManager& m_Assets;
        Veng::Input& m_Input;
        Veng::InputRouter& m_Router;
        Veng::SystemRegistry& m_Systems;

        Veng::AssetHandle<Veng::Prefab> m_Prefab;

        /// @brief The authored scene, edited while not playing and cloned to start a play session.
        Veng::Unique<Veng::Scene> m_Scene;

        /// @brief The world the play session runs in, or invalid while editing.
        ///
        /// Held by id, never by scene address: the runner owns the world, and a world closed under
        /// the session simply stops resolving.
        Veng::WorldInstanceId m_PlayWorld;

        /// @brief The pause the toolbar's Pause holds on the play world; inert when not held.
        Veng::WorldPauseScope m_PlayPause;

        /// @brief The viewport registered as the play world's presentation, or null when none is.
        Veng::Renderer::Viewport* m_PlayPresentation = nullptr;
    };
}
