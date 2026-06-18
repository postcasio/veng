#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    class PanelHost;

    // Lists the asset table of a mounted .vengpack, read-only. Selecting an asset
    // records the selection (consumed by the inspector); double-clicking asks the
    // host to open the type's registered editor (no-op for an unregistered type).
    class AssetBrowserPanel final : public EditorPanel
    {
    public:
        AssetBrowserPanel(Veng::path packPath, PanelHost& host);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Asset Browser"; }
        void OnImGui() override;

    private:
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
        bool m_Loaded = false;

        void LoadTable();
    };
}
