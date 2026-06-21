#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Veng.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    class PanelHost;
    class AssetSourceIndex;

    /// @brief Windows-Explorer-style browser over a mounted .vengpack.
    ///
    /// A directory tree (left) built from the pack manifest's source paths drives a content
    /// view (right) with three switchable layouts — a detail list, a multi-column list, and
    /// an icon grid. Selecting an asset records the selection (consumed by the inspector);
    /// double-clicking an asset asks the host to open the type's registered editor (no-op for
    /// an unregistered type), and double-clicking a folder descends into it. Every asset is a
    /// drag source carrying an AssetDragPayload, droppable onto an inspector AssetHandle field.
    class AssetBrowserPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the browser for the pack at @p packPath.
        /// @param packPath  Path to the mounted .vengpack archive.
        /// @param sources   Manifest source index supplying folder paths and display names.
        /// @param host      Host the panel asks to open asset editors.
        AssetBrowserPanel(Veng::path packPath, const AssetSourceIndex& sources, PanelHost& host);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Asset Browser"; }
        void OnImGui() override;

    private:
        /// @brief How the content view lays out the current folder's items.
        enum class ViewMode
        {
            /// @brief One full-width row per item.
            DetailList,
            /// @brief A table with Name / Type / Size / Id columns.
            Columns,
            /// @brief A flow grid of icon tiles.
            Icons,
        };

        /// @brief One asset, resolved from the pack TOC and the manifest source index.
        struct AssetEntry
        {
            /// @brief The asset's id.
            Veng::AssetId Id;
            /// @brief The asset's type.
            Veng::AssetType Type = Veng::AssetType::Raw;
            /// @brief Byte size of the cooked blob, from the TOC.
            Veng::u64 Size = 0;
            /// @brief Display name (source filename with the cooked extension stripped).
            Veng::string Name;
        };

        /// @brief A folder node in the source-path tree.
        struct FolderNode
        {
            /// @brief Folder name (the root's name is the pack filename).
            Veng::string Name;
            /// @brief Subfolders, keyed and ordered by name.
            Veng::map<Veng::string, FolderNode> Children;
            /// @brief Assets sitting directly in this folder.
            Veng::vector<AssetEntry> Assets;
        };

        Veng::path m_PackPath;
        const AssetSourceIndex& m_Sources;
        PanelHost& m_Host;

        /// @brief The folder tree, root at the pack.
        FolderNode m_Root;
        /// @brief True once the pack TOC has been read into the tree.
        bool m_Loaded = false;

        /// @brief Active content-view layout.
        ViewMode m_ViewMode = ViewMode::Columns;
        /// @brief Path (folder names from the root) of the folder shown on the right.
        Veng::vector<Veng::string> m_CurrentFolder;
        /// @brief The selected asset, consumed by the inspector.
        Veng::optional<Veng::AssetId> m_Selected;
        /// @brief Case-insensitive name filter applied to the content view.
        Veng::string m_Filter;

        /// @brief Reads the pack TOC into the folder tree; called once on first OnImGui.
        void LoadTable();
        /// @brief Returns the node named by m_CurrentFolder, falling back to root on a stale path.
        FolderNode& CurrentFolder();
        /// @brief Draws the left directory tree rooted at @p node.
        /// @param node  Folder to draw.
        /// @param path  Accumulated path from the root to @p node (mutated during recursion).
        void DrawTree(FolderNode& node, Veng::vector<Veng::string>& path);
        /// @brief Draws the right content view for the current folder.
        void DrawContent();
    };
}
