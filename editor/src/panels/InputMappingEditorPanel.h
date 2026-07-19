#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input/Actions.h>

#include "AssetEditorPanel.h"
#include "AssetSaveModel.h"

#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    class Input;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Docked panel for viewing and editing a .inputmap.json binding table.
    ///
    /// Draws the input map's reflected document — its `vector<InputAction>` actions and its
    /// `vector<Binding>` bindings — through the shared reflection inspector (`DrawFields`), so the
    /// binding table is add/remove/edit-able with no bespoke widget code. The one custom widget is
    /// an `ActionId` name combo scoped to the document's own declared actions, so a binding shows
    /// and picks its action by name rather than a raw numeric id. A read-only preview readout
    /// resolves the document against the editor's own input each frame, so a binding's effect is
    /// observable without launching the game.
    ///
    /// It writes nothing until an explicit save: an edit marks the document dirty, and Save
    /// performs the preserve-unknown-keys merge write, then the recook that hot-reloads behind the
    /// stable `AssetHandle`. It is deliberately basic: no press-a-key-to-bind capture and no
    /// drag-reorder.
    class InputMappingEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the input map at @p id / @p sourcePath.
        /// @param id         The input map's AssetId, the recook target and hot-reload handle key.
        /// @param sourcePath The .inputmap.json source the panel reads, writes, and recooks.
        /// @param assets     Asset manager supplying the TypeRegistry the inspector walks and the
        ///                   hot-reload handle.
        /// @param editors    Editor registry the ActionId field widget registers into.
        /// @param sources    Manifest source index the inspector's asset pickers read.
        /// @param input      The editor host's always-fed input snapshot the preview resolves over.
        /// @param cook       Cook-on-demand driver bound to EditorHost::RequestCook.
        InputMappingEditorPanel(Veng::AssetId id, Veng::path sourcePath, Veng::AssetManager& assets,
                                Veng::EditorRegistry& editors, const AssetSourceIndex& sources,
                                const Veng::Input& input, CookDriver cook);
        ~InputMappingEditorPanel() override;

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

        /// @brief Writes the actions and bindings back to the .inputmap.json, then recooks.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true while the document holds edits not yet written to the source.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Dirty; }

        /// @brief Exposes the reflected document so the generic editor MCP tools can read/write it.
        [[nodiscard]] Veng::vector<Inspectable> GetInspectables() override;

        /// @brief Marks the document dirty after an external write, matching a UI edit's reaction.
        void OnInspectableChanged(Veng::string_view name) override;

    private:
        /// @brief Reads the on-disk .inputmap.json into m_Doc; absent fields keep defaults.
        void LoadDocument();

        /// @brief Writes the document back to the .inputmap.json, preserving unknown keys.
        /// @return Empty on success; an I/O error otherwise.
        [[nodiscard]] Veng::VoidResult WriteDocument();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Draws the read-only resolved-action preview readout for the editor's input.
        void DrawPreview();

        /// @brief Draws the ActionId name combo scoped to the document's declared actions.
        ///
        /// The one custom field widget: an ActionId is a u64 leaf with no default scalar widget, so
        /// the generic path draws it disabled. This combo picks an action by name from m_Doc.Actions.
        /// @param fieldPtr Pointer to the ActionId field bytes.
        void DrawActionCombo(void* fieldPtr);

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;

        Veng::AssetManager& m_Assets;
        const AssetSourceIndex& m_Sources;
        const Veng::EditorRegistry& m_Editors;
        const Veng::Input& m_Input;
        CookDriver m_Cook;

        /// @brief The reflected document the inspector draws — the source actions + bindings.
        Veng::InputMapData m_Doc;

        Veng::AssetHandle<Veng::InputMappingContext> m_Handle;
        Veng::MountHandle m_Mount;

        /// @brief The resolver-ready form of m_Doc, rebuilt each frame for the preview readout.
        Veng::ResolvedContext m_Resolved;

        /// @brief The prior frame's resolved actions, threaded as `previous` for phase derivation.
        Veng::ActionState m_Previous;

        /// @brief Serialises recooks; a save landing behind one in flight queues rather than drops.
        CookGate m_Gate;

        /// @brief Whether the document holds edits not yet written to the source.
        bool m_Dirty = false;

        Veng::optional<Veng::string> m_CookError;
    };
}
