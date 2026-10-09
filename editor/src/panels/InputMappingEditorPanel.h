#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input/Actions.h>
#include <Veng/Path.h>
#include <Veng/Result.h>

#include <VengEditor/AssetEditorPanel.h>
#include <VengEditor/AssetSaveModel.h>

#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    class Input;
    class TypeRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Reads an .inputmap.json into its reflected document.
    ///
    /// Binds through the shared JSON walker, so every reflected action and binding field reads by
    /// its field name. An absent field keeps its default and a key naming no field is ignored, the
    /// editor's tolerant posture; the cook rejects such a key.
    /// @param source  The .inputmap.json to read.
    /// @param types   Registry carrying InputMapData and its element types.
    /// @return The document, or an error naming the file and the malformed field.
    [[nodiscard]] Veng::Result<Veng::InputMapData>
    ReadInputMapDocument(const Veng::path& source, const Veng::TypeRegistry& types);

    /// @brief Writes a reflected input-map document back to its .inputmap.json.
    ///
    /// Assigns every reflected field into the existing file, so a key the document does not own (a
    /// hand-authored note) survives; the inverse of ReadInputMapDocument.
    /// @param source  The .inputmap.json to write.
    /// @param data    The document.
    /// @param types   Registry carrying InputMapData and its element types.
    /// @return Empty on success; an I/O error otherwise.
    [[nodiscard]] Veng::VoidResult WriteInputMapDocument(const Veng::path& source,
                                                         const Veng::InputMapData& data,
                                                         const Veng::TypeRegistry& types);

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
    /// The document reads and writes through the same reflection walker, so every reflected field
    /// of an action or binding round-trips with no panel code of its own.
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
        /// @param editors    Editor registry whose custom widgets the inspector draws; read only.
        /// @param sources    Manifest source index the inspector's asset pickers read.
        /// @param input      The editor host's always-fed input snapshot the preview resolves over.
        /// @param cook       Cook-on-demand driver bound to EditorHost::RequestCook.
        InputMappingEditorPanel(Veng::AssetId id, Veng::path sourcePath, Veng::AssetManager& assets,
                                const Veng::EditorRegistry& editors,
                                const AssetSourceIndex& sources, const Veng::Input& input,
                                CookDriver cook);
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
        /// @brief Reads the on-disk .inputmap.json into m_Doc through ReadInputMapDocument.
        void LoadDocument();

        /// @brief Writes m_Doc back to the .inputmap.json through WriteInputMapDocument.
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
        /// It is supplied to the panel's own inspector walk as a FieldWidgetOverride, never
        /// registered on the shared EditorRegistry, so it draws only this document's fields.
        /// @param fieldPtr Pointer to the ActionId field bytes.
        /// @return True when the pick changed the field.
        bool DrawActionCombo(void* fieldPtr) const;

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
