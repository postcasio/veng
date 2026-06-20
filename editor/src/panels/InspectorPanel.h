#pragma once

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Scene/Entity.h>

#include <VengEditor/EditorPanel.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class EditorRegistry;
    class TypeRegistry;
    struct TypeInfo;
}

namespace VengEditor
{
    class AssetSourceIndex;

    /// @brief Reflection-driven property inspector for the selected entity.
    ///
    /// Walks the selected entity's components through the TypeRegistry, drawing a
    /// widget per FieldClass for every reflected field. A custom widget registered
    /// in the EditorRegistry overrides the built-in for a given field TypeId.
    class InspectorPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the inspector with the required asset/editor context.
        InspectorPanel(Veng::AssetManager& assets, Veng::EditorRegistry& editors,
                       const AssetSourceIndex& sources);

        [[nodiscard]] Veng::string_view GetTitle() const override { return "Inspector"; }
        void OnImGui() override;

        /// @brief Sets the live scene and selection; call each frame before OnImGui.
        /// @param scene   The scene the inspector reads, or null when no scene is live.
        /// @param entity  The selected entity, or nullopt to show "Nothing selected".
        void SetSelection(Veng::Scene* scene, Veng::optional<Veng::Entity> entity)
        {
            m_Scene = scene;
            m_Selected = entity;
        }

    private:
        /// @brief Draws field widgets for all visible fields of @p type over @p base.
        void DrawFields(void* base, const Veng::TypeInfo& type);

        Veng::AssetManager& m_Assets;
        Veng::EditorRegistry& m_Editors;
        const AssetSourceIndex& m_Sources;

        Veng::Scene* m_Scene = nullptr;
        Veng::optional<Veng::Entity> m_Selected;
    };
}
