#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
}

namespace VengEditor
{
    // Lists the asset table of a mounted .vengpack, read-only. Selecting an asset
    // records the selection (consumed by the inspector); double-clicking opens the
    // type's registered editor (no-op for an unregistered type).
    class AssetBrowserPanel final : public EditorPanel
    {
    public:
        AssetBrowserPanel(Veng::path packPath, Veng::EditorRegistry& editors);

        [[nodiscard]] Veng::string_view Title() const override { return "Asset Browser"; }
        void OnImGui() override;

        // Panels opened by double-clicking an asset since the last call, moved out
        // for the host to adopt into its panel set. Drained once per frame.
        [[nodiscard]] Veng::vector<Veng::Unique<EditorPanel>> TakeOpenedPanels();

    private:
        struct Asset
        {
            Veng::AssetId Id;
            Veng::AssetType Type = Veng::AssetType::Raw;
            Veng::u64 Size = 0;
        };

        Veng::path m_PackPath;
        Veng::EditorRegistry& m_Editors;
        Veng::vector<Asset> m_Assets;
        Veng::optional<Veng::AssetId> m_Selected;
        Veng::vector<Veng::Unique<EditorPanel>> m_Opened;
        bool m_Loaded = false;

        void LoadTable();
    };
}
