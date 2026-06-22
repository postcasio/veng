#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Scene/SceneSystem.h>

#include "AssetEditorPanel.h"
#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    class ImGuiLayer;
    class Input;
    class InputRouter;
    class Prefab;
    class Scene;
    class SceneSimulation;
    class SystemRegistry;
    class TypeRegistry;

    namespace Renderer
    {
        class CommandBuffer;
        class Context;
    }
}

namespace VengEditor
{
    class AssetSourceIndex;
    class SceneViewportPanel;

    /// @brief Asset editor for a prefab: a private dockspace hosting a scene viewport,
    /// an entity-hierarchy explorer, and a reflection inspector over one spawned Scene.
    ///
    /// On open the prefab is loaded and spawned into a fresh Scene the document owns; a
    /// default directional light is added when the prefab carries none so the content is
    /// lit. The explorer drives selection, the inspector edits the selected entity's
    /// components, and the viewport renders the live scene — all sharing one
    /// PrefabEditContext.
    class PrefabEditorPanel : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the prefab at @p id.
        /// @param id        The prefab asset to edit.
        /// @param context   Render context for the viewport's SceneRenderer.
        /// @param assets    Asset manager the prefab and its dependencies load through.
        /// @param imgui     ImGui layer the viewport registers its render target with.
        /// @param types     Type registry the spawned Scene pools components against.
        /// @param editors   Editor registry for inspector field-widget overrides.
        /// @param sources   Manifest source index for the inspector's asset pickers.
        /// @param input     Frame-coherent input service the viewport camera reads.
        /// @param router    Input router whose gameplay focus captures the mouse during Play.
        /// @param systems   System registry the play session instantiates its systems from.
        PrefabEditorPanel(Veng::AssetId id, Veng::Renderer::Context& context,
                          Veng::AssetManager& assets, Veng::ImGuiLayer& imgui,
                          Veng::TypeRegistry& types, Veng::EditorRegistry& editors,
                          const AssetSourceIndex& sources, Veng::Input& input,
                          Veng::InputRouter& router, Veng::SystemRegistry& systems);
        ~PrefabEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }

        /// @brief Clones the edit scene and starts a play session running its systems over the clone.
        ///
        /// Repoints the shared context at the clone, builds the SceneSimulation on first
        /// play, calls each system's OnStart, and clears the selection (its handles point
        /// into the edit scene). The simulation runs the set GetPlaySystems() names — every
        /// registered system for a prefab document, the level's ordered set for a level
        /// document. A no-op while already playing.
        void Play();

        /// @brief Stops the play session: calls OnStop, drops the clone, and restores the edit scene.
        ///
        /// Repoints the shared context back at the edit scene, clears the selection, and
        /// returns to Editing. A no-op while not playing.
        void Stop();

        /// @brief Pauses an active play session, holding the clone without advancing it. No-op unless Playing.
        void Pause();

        /// @brief Resumes a paused play session. No-op unless Paused.
        void Resume();

        /// @brief Ticks the play simulation (when playing), then forwards the render to the children.
        ///
        /// Advances the clone's systems before the base forwards OnRender so the viewport
        /// renders the just-updated frame.
        /// @param cmd  Command buffer for the current frame.
        void OnRender(Veng::Renderer::CommandBuffer& cmd) override;

        /// @brief Draws the document toolbar above the dockspace: a live entity-count readout.
        void OnUI() override;

    protected:
        /// @brief Constructs the document over a world prefab id, deferring child wiring to a subclass.
        ///
        /// Builds the edit Scene from @p worldPrefab but adds no child panels — a subclass
        /// (the level editor) calls AddSceneEditingChildren and adds its own panels, then
        /// arranges them in its own BuildDefaultLayout. Used to compose the scene-editing
        /// surface into a richer editor without reimplementing it.
        /// @param worldPrefab The prefab spawned into the edit scene.
        /// @param title       The document window title.
        /// @param context     Render context for the viewport's SceneRenderer.
        /// @param assets       Asset manager the prefab and its dependencies load through.
        /// @param imgui        ImGui layer the viewport registers its render target with.
        /// @param types        Type registry the spawned Scene pools components against.
        /// @param editors      Editor registry for inspector field-widget overrides.
        /// @param sources      Manifest source index for the inspector's asset pickers.
        /// @param input        Frame-coherent input service the viewport camera reads.
        /// @param router       Input router whose gameplay focus captures the mouse during Play.
        /// @param systems      System registry the play session instantiates its systems from.
        PrefabEditorPanel(Veng::AssetId worldPrefab, Veng::string title,
                          Veng::Renderer::Context& context, Veng::AssetManager& assets,
                          Veng::ImGuiLayer& imgui, Veng::TypeRegistry& types,
                          Veng::EditorRegistry& editors, const AssetSourceIndex& sources,
                          Veng::Input& input, Veng::InputRouter& router,
                          Veng::SystemRegistry& systems);

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

        /// @brief Adds the viewport / explorer / inspector children over the shared edit context.
        ///
        /// Called by a subclass after the base constructor has built the scene, so the level
        /// editor composes the same scene-editing surface a standalone prefab editor uses.
        /// @param context  Render context the viewport's SceneRenderer is created against.
        /// @param imgui    ImGui layer the viewport registers its render target with.
        /// @param editors  Editor registry for inspector field-widget overrides.
        /// @param sources  Manifest source index for the inspector's asset pickers.
        void AddSceneEditingChildren(Veng::Renderer::Context& context, Veng::ImGuiLayer& imgui,
                                     Veng::EditorRegistry& editors,
                                     const AssetSourceIndex& sources);

        /// @brief The shared edit context the scene-editing children and a subclass operate over.
        PrefabEditContext m_Context;

        Veng::usize m_ExplorerChild = 0;
        Veng::usize m_ViewportChild = 0;
        Veng::usize m_InspectorChild = 0;

        /// @brief The viewport child instance, for a subclass to drive renderer-facing state (e.g. level render settings).
        SceneViewportPanel* m_Viewport = nullptr;

    private:
        /// @brief Loads and spawns the prefab, adding a default light when none is present.
        void BuildScene(Veng::Renderer::Context& context, Veng::AssetManager& assets);

        /// @brief Pushes gameplay input focus (capturing the cursor) if not already held.
        void CaptureForPlay();
        /// @brief Pops gameplay input focus (releasing the cursor) if currently held.
        void ReleaseFromPlay();

        Veng::AssetId m_Id;
        Veng::string m_Title;

        Veng::AssetManager& m_Assets;
        Veng::Input& m_Input;
        Veng::InputRouter& m_Router;
        Veng::SystemRegistry& m_Systems;

        Veng::AssetHandle<Veng::Prefab> m_Prefab;

        /// @brief The authored scene, edited while not playing and cloned to start a play session.
        Veng::Unique<Veng::Scene> m_Scene;

        /// @brief The throwaway play clone, non-null only during a play session.
        Veng::Unique<Veng::Scene> m_PlayScene;

        /// @brief The system driver, built lazily on the first Play and reused across sessions.
        Veng::Unique<Veng::SceneSimulation> m_Simulation;
    };
}
