#pragma once

#include <Veng/Reflection/FieldDescriptor.h>

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
    // The property inspector. Walks the selected entity's components through the
    // TypeRegistry, drawing a widget per FieldClass for every reflected field. The
    // host sets the scene and selection each frame; a custom widget registered in
    // the EditorRegistry overrides the built-in for a given field TypeId.
    class InspectorPanel final : public EditorPanel
    {
    public:
        InspectorPanel(Veng::AssetManager& assets, Veng::EditorRegistry& editors);

        [[nodiscard]] Veng::string_view Title() const override { return "Inspector"; }
        void OnImGui() override;

        // Set by the host each frame: the live scene the inspector reads and the
        // selected entity (nullopt → "Nothing selected").
        void SetSelection(Veng::Scene* scene, Veng::optional<Veng::Entity> entity)
        {
            m_Scene = scene;
            m_Selected = entity;
        }

    private:
        void DrawFields(void* base, const Veng::TypeInfo& type);
        void DrawField(void* fieldPtr, const Veng::FieldDescriptor& field);

        Veng::AssetManager& m_Assets;
        Veng::EditorRegistry& m_Editors;

        Veng::Scene* m_Scene = nullptr;
        Veng::optional<Veng::Entity> m_Selected;
    };
}
