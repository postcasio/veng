#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include "AssetEditorPanel.h"
#include "panels/TableDocument.h"
#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class AssetManager;
    class TypeRegistry;
}

namespace VengEditor
{
    /// @brief Docked authoring panel for a `*.tableschema.json` column schema.
    ///
    /// Edits the column list — add, remove, rename, retype, reorder — and the key column, with the
    /// resolved row layout and every validation failure shown live. Validation is the importer's
    /// own (Veng::LayOutTableSchema), so what this panel accepts is what the cook accepts.
    ///
    /// It writes nothing until an explicit save. Editing marks the document dirty; Save performs
    /// the preserve-unknown-keys merge write, then the recook, in that order — so a schema edit
    /// that breaks a table cooked against it leaves the saved source on disk and reports the cook
    /// failure in-panel rather than silently reverting.
    class TableSchemaEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the schema at @p id / @p sourcePath.
        /// @param id          The TableSchema asset id, addressable behind the cook-on-demand mount.
        /// @param sourcePath  Absolute path to the `*.tableschema.json` source.
        /// @param types       Registry the column types resolve and are picked from.
        /// @param cook        Off-thread cook driver bound to the host's RequestCook.
        TableSchemaEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                               const Veng::TypeRegistry& types, CookDriver cook);

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

        /// @brief Writes the column list back to the `*.tableschema.json`, then recooks.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true while the column list holds edits not yet written to the source.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Dirty; }

    private:
        /// @brief Reads the on-disk source into the document and re-validates.
        void LoadDocument();

        /// @brief Re-resolves the document, refreshing the layout read-out and the error banner.
        void Revalidate();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Draws the column table; returns true when an edit changed the document.
        [[nodiscard]] bool DrawColumns();

        /// @brief Draws the key-column combo, restricted to columns a key index can order.
        [[nodiscard]] bool DrawKeyColumn();

        /// @brief Draws a searchable type picker popup; returns the chosen type, or null.
        /// @param id       ImGui id of the popup, unique per call site.
        /// @param filter   Caller-owned search text, held across frames.
        [[nodiscard]] const Veng::TypeInfo* DrawTypePicker(Veng::string_view id,
                                                           Veng::string& filter);

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;
        const Veng::TypeRegistry& m_Types;
        CookDriver m_Cook;

        TableSchemaDocument m_Document;

        /// @brief The resolved layout of the current document, or nullopt while it is invalid.
        Veng::optional<Veng::TableSchemaLayout> m_Layout;
        /// @brief The laid-out column descriptors behind m_Layout; empty while the document is invalid.
        Veng::vector<Veng::TableColumnDescriptor> m_Resolved;
        /// @brief Why the current document is not a legal schema; empty when it is.
        Veng::string m_ValidationError;

        /// @brief Search text of the add-column type picker, held across frames.
        Veng::string m_AddFilter;
        /// @brief Search text of the retype picker, held across frames.
        Veng::string m_RetypeFilter;
        /// @brief Index of the column the retype picker is open for.
        Veng::usize m_RetypeColumn = 0;

        /// @brief Whether the document holds edits not yet written to the source.
        bool m_Dirty = false;
        /// @brief Whether an edit since the last save removed or retyped a column.
        ///
        /// Either invalidates every table already cooked against this schema, which is the table's
        /// cook error to surface — so this warns rather than blocks.
        bool m_Destructive = false;

        /// @brief Cook submitted but not yet mounted; suppresses concurrent cooks.
        bool m_Cooking = false;
        /// @brief A save landed while a cook was in flight; re-cooks once that one completes.
        bool m_CookQueued = false;
        Veng::optional<Veng::string> m_CookError;
        Veng::MountHandle m_Mount;
    };
}
