#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneSystem.h>

#include <VengEditor/CookRequest.h>

#include "panels/PrefabEditorPanel.h"
#include "panels/TextureEditorPanel.h"

namespace Veng
{
    class Application;
    class SystemRegistry;
}

namespace VengEditor
{
    /// @brief Asset editor for a Level: the world-prefab scene surface plus level-scoped wiring.
    ///
    /// A level wraps a world prefab with the data that is not reusable-recipe data — the
    /// ordered active system set, the game-mode/Session config, and the render settings.
    /// This editor composes the prefab editing surface (it derives from PrefabEditorPanel,
    /// so the viewport / explorer / inspector edit the world prefab without reimplementing
    /// scene editing) and adds two level-scoped children:
    ///
    /// - a systems panel listing the SystemRegistry catalog with an enable toggle and
    ///   drag-reorder over the active set, writing the level's ordered systems list, and
    /// - a settings panel drawing the game-mode and render config through the shared
    ///   reflection inspector (DrawFieldWidget).
    ///
    /// Editing the level recooks the *.level.json off the render thread and hot-reloads the
    /// asset behind its stable handle, round-tripping the JSON (patching known keys,
    /// preserving unknown ones). Play runs exactly the level's ordered system set through
    /// the base's play machinery (GetPlaySystems), distinct from a prefab document's
    /// "all registered" set.
    class LevelEditorPanel final : public PrefabEditorPanel
    {
    public:
        /// @brief Opens the level editor for the level at @p id with world prefab @p worldPrefab.
        /// @param id          The level asset being edited.
        /// @param worldPrefab The level's world prefab, opened in the scene surface.
        /// @param sourcePath  The *.level.json source the editor round-trips and recooks.
        /// @param app         Application the viewport registers its Offscreen viewport into.
        /// @param assets      Asset manager the level and its dependencies load through.
        /// @param imgui       ImGui layer the viewport registers its render target with.
        /// @param types       Type registry the spawned Scene and config structs reflect against.
        /// @param editors     Editor registry for inspector field-widget overrides.
        /// @param sources     Manifest source index for the inspector's asset pickers.
        /// @param input       Frame-coherent input service the viewport camera reads.
        /// @param router      Input router whose gameplay focus captures the mouse during Play.
        /// @param systems     System registry the systems panel lists and Play instantiates from.
        /// @param cook        Cook driver that recooks the level source and shadow-mounts the result.
        LevelEditorPanel(Veng::AssetId id, Veng::AssetId worldPrefab, Veng::path sourcePath,
                         Veng::Application& app, Veng::AssetManager& assets,
                         Veng::ImGuiLayer& imgui, Veng::TypeRegistry& types,
                         Veng::EditorRegistry& editors, const AssetSourceIndex& sources,
                         Veng::Input& input, Veng::InputRouter& router,
                         Veng::SystemRegistry& systems, CookDriver cook);
        ~LevelEditorPanel() override;

        /// @brief Draws the document toolbar: the shared play/gizmo transport plus the active-system-set readout.
        void OnUI() override;

        /// @brief Saves the world prefab's entity edits and the level's own config, then recooks.
        ///
        /// Extends the base prefab save (the world .prefab.json) with the level's .level.json
        /// config (systems + game-mode + render): whichever side is dirty is written, then a
        /// recook re-mounts the Level behind its stable handle. Config edits accumulate in memory
        /// until this runs — nothing is written per-edit.
        /// @return Empty on success; an error string on an I/O or save failure.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true when the scene edits or the level config have unsaved changes.
        [[nodiscard]] bool HasUnsavedChanges() const override
        {
            return m_Commands.IsDirty() || m_ConfigDirty;
        }

    protected:
        /// @brief Splits the dockspace: explorer + systems (left), viewport (center), inspector + settings (right).
        void BuildDefaultLayout(Veng::u32 dockspaceId) override;

        /// @brief Returns the level's ordered active system set, so Play runs exactly it.
        [[nodiscard]] const Veng::vector<Veng::SystemId>* GetPlaySystems() const override
        {
            return &m_Systems;
        }

        /// @brief Seeds the play clone with the level's settings entity, player prefab made resident.
        ///
        /// Drives the play clone to the same initialized state Level::LoadInto reaches at
        /// runtime: SeedLevel adds the Playing Session, game-mode config, and render settings the
        /// systems and engine read, and the game-mode player prefab is forced resident (a
        /// LoadSync, skipped when already loaded) so the rule has something to spawn at Start.
        /// @param scene The play clone the level systems run over.
        void SeedPlayScene(Veng::Scene& scene) override;

    private:
        /// @brief Reads the *.level.json into m_Systems / m_GameMode / m_Render; absent keys keep defaults.
        void LoadConfig();

        /// @brief Patches the systems/gameMode/render keys in the existing JSON (preserving
        /// unknown keys) and writes it back with 4-space indent.
        /// @return False (error recorded) on I/O or parse failure.
        bool SaveConfig();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Flags the level config dirty so the next Save persists and recooks it.
        void MarkDirty();

        /// @brief Draws the systems-catalog child: enable toggles, drag-reorder, phase labels.
        void DrawSystemsPanel();

        /// @brief Draws the level-settings child: game-mode and render config via the inspector.
        void DrawSettingsPanel();

        /// @brief Child panel forwarding its draw back to one of the level editor's draw methods.
        class LevelChildPanel;

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;

        Veng::AssetManager& m_AssetManager;
        const Veng::SystemRegistry& m_Catalog;
        Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;
        CookDriver m_Cook;

        /// @brief The level's ordered active system set; Play runs exactly this.
        Veng::vector<Veng::SystemId> m_Systems;
        /// @brief The game-mode/Session config edited through the reflection inspector.
        Veng::GameModeConfig m_GameMode;
        /// @brief The render-settings subset edited through the reflection inspector.
        Veng::LevelRenderSettings m_Render;

        /// @brief The reloaded level handle, re-fetched behind the stable handle on each recook.
        Veng::AssetHandle<Veng::Level> m_Handle;
        /// @brief The shadow mount of the latest cook; dropped/replaced on the next recook.
        Veng::MountHandle m_Mount;

        /// @brief Cook submitted but not yet mounted; suppresses concurrent cooks.
        bool m_Cooking = false;
        /// @brief Unsaved config edits (systems / game-mode / render) await the next Save.
        bool m_ConfigDirty = false;
        Veng::optional<Veng::string> m_CookError;

        Veng::usize m_SystemsChild = 0;
        Veng::usize m_SettingsChild = 0;
    };
}
