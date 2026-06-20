#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    class PanelHost;

    /// @brief Lists the asset table of a mounted .vengpack, read-only.
    ///
    /// Selecting an asset records the selection (consumed by the inspector);
    /// double-clicking asks the host to open the type's registered editor
    /// (no-op for an unregistered type).
    class AssetBrowserPanel final : public EditorPanel
    {
    public:
        /// @brief Opens the browser for the pack at @p packPath.
        AssetBrowserPanel(Veng::path packPath, PanelHost& host);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Asset Browser"; }
        void OnImGui() override;

    private:
        /// @brief Flat entry read from the pack TOC.
        struct Asset
        {
            Veng::AssetId Id;
            Veng::AssetType Type = Veng::AssetType::Raw;
            Veng::u64 Size = 0;
        };

        Veng::path m_PackPath;
        PanelHost& m_Host;
        Veng::vector<Asset> m_Assets;
        Veng::optional<Veng::AssetId> m_Selected;
        /// @brief True once the pack TOC has been read into m_Assets.
        bool m_Loaded = false;

        /// @brief Reads the pack TOC into m_Assets; called once on first OnImGui.
        void LoadTable();
    };
}
