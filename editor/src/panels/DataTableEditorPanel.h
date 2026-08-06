#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <VengEditor/AssetEditorPanel.h>
#include "panels/TableDocument.h"
#include "panels/TextureEditorPanel.h" // CookDriver alias

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    class TypeRegistry;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Opens another asset's editor — the schema link in the table editor's header.
    using AssetOpener = Veng::function<void(Veng::AssetTypeId, Veng::AssetId)>;

    /// @brief Docked authoring panel for a `*.table.json` row set, validated against its schema.
    ///
    /// A grid with one column per schema column. A cell is drawn by the inspector widget for the
    /// column's reflected type, so an enum column gets the named combo and an asset-handle column
    /// the asset picker with no per-column widget written here. Rows add, remove, duplicate, and
    /// reorder; key-column cells are marked live when they duplicate an earlier row's, the rule the
    /// importer rejects a cook for. The row body virtualizes, so a table far past the visible rows
    /// costs only what is on screen.
    ///
    /// It writes nothing until an explicit save. Save performs the preserve-unknown-keys merge
    /// write, then the recook, then the hot reload behind the stable handle — in that order, so a
    /// cook that fails (against a schema changed underneath it, say) leaves the saved source on
    /// disk and reports here rather than reverting the edit.
    class DataTableEditorPanel final : public AssetEditorPanel
    {
    public:
        /// @brief Opens the editor for the table at @p id / @p sourcePath.
        /// @param id          The DataTable asset id, addressable behind the cook-on-demand mount.
        /// @param sourcePath  Absolute path to the `*.table.json` source.
        /// @param sources     Manifest index resolving the schema reference back to its source file.
        /// @param assets      Asset manager the cooked table reloads through.
        /// @param editors     Editor registry supplying per-type custom cell widgets.
        /// @param cook        Off-thread cook driver bound to the host's RequestCook.
        /// @param openAsset   Opens another asset's editor; the header's schema link.
        DataTableEditorPanel(Veng::AssetId id, Veng::path sourcePath,
                             const AssetSourceIndex& sources, Veng::AssetManager& assets,
                             const Veng::EditorRegistry& editors, CookDriver cook,
                             AssetOpener openAsset);

        [[nodiscard]] Veng::string_view GetTitle() const override { return m_Title; }
        void OnUI() override;

        /// @brief Writes the rows back to the `*.table.json`, then recooks and hot-reloads.
        [[nodiscard]] Veng::VoidResult Save() override;

        /// @brief Returns true while the rows hold edits not yet written to the source.
        [[nodiscard]] bool HasUnsavedChanges() const override { return m_Dirty; }

    private:
        /// @brief Reads the on-disk source and its schema into the document.
        ///
        /// Resolves the schema reference through the source index and re-parses that schema's own
        /// JSON — the editor holds no cooked schema, and the authored source is what a cell must
        /// bind against.
        void LoadDocument();

        /// @brief Submits a recook of the current on-disk source through the cook driver.
        void TriggerCook();

        /// @brief Draws the row grid; returns true when an edit changed a cell.
        [[nodiscard]] bool DrawGrid();

        /// @brief Draws one cell's widget, or a summary opening the composite editor popup.
        /// @param row     Index of the row the cell belongs to.
        /// @param column  Index of the column within the resolved schema.
        /// @return True when the edit changed the cell.
        [[nodiscard]] bool DrawCell(Veng::usize row, Veng::usize column);

        Veng::AssetId m_Id;
        Veng::path m_SourcePath;
        Veng::string m_Title;
        const AssetSourceIndex& m_Sources;
        Veng::AssetManager& m_Assets;
        const Veng::EditorRegistry& m_Editors;
        CookDriver m_Cook;
        AssetOpener m_OpenAsset;

        TableDataDocument m_Document;

        /// @brief The schema's laid-out columns; empty when the schema did not resolve.
        Veng::vector<Veng::TableColumnDescriptor> m_Columns;
        /// @brief The schema's row layout; unset when the schema did not resolve.
        Veng::optional<Veng::TableSchemaLayout> m_Layout;
        /// @brief Why the schema could not be resolved; empty when it was.
        Veng::string m_SchemaError;
        /// @brief Per-cell binding failures from the last load; each names its row and column.
        Veng::vector<Veng::string> m_Diagnostics;

        /// @brief Per row, whether its key cell duplicates an earlier row's; recomputed each frame.
        Veng::vector<bool> m_DuplicateKeys;

        /// @brief Whether the document holds edits not yet written to the source.
        bool m_Dirty = false;

        /// @brief Cook submitted but not yet mounted; suppresses concurrent cooks.
        bool m_Cooking = false;
        /// @brief A save landed while a cook was in flight; re-cooks once that one completes.
        bool m_CookQueued = false;
        Veng::optional<Veng::string> m_CookError;
        Veng::MountHandle m_Mount;
        /// @brief The stable table handle; re-fetched behind each fresh cook-on-demand mount.
        Veng::AssetHandle<Veng::DataTable> m_Handle;
    };
}
