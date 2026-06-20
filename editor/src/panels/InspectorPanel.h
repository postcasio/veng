#pragma once

#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

#include "panels/PrefabEditContext.h"

namespace Veng
{
    class AssetManager;
    class EditorRegistry;
    struct TypeInfo;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Reflection-driven property inspector for the prefab editor's selection.
    ///
    /// Reads the selected entity from the shared PrefabEditContext and walks its
    /// components through the TypeRegistry, drawing a widget per FieldClass for every
    /// reflected field. A custom widget registered in the EditorRegistry overrides the
    /// built-in for a given field TypeId.
    class InspectorPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the inspector over the document's edit context.
        /// @param assets   Asset manager for resolving handle widgets.
        /// @param editors  Editor registry for field-widget overrides.
        /// @param sources  Manifest source index for the asset pickers.
        /// @param ctx      Shared document context supplying the Scene and selection.
        InspectorPanel(Veng::AssetManager& assets, Veng::EditorRegistry& editors,
                       const AssetSourceIndex& sources, PrefabEditContext& ctx);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Inspector"; }
        void OnImGui() override;

    private:
        /// @brief Draws field widgets for all visible fields of @p type over @p base.
        void DrawFields(void* base, const Veng::TypeInfo& type);

        Veng::AssetManager& m_Assets;
        Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;
        PrefabEditContext& m_Ctx;
    };
}
