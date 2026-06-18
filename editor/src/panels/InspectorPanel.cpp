#include "InspectorPanel.h"

#include "FieldWidget.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

namespace VengEditor
{
    using namespace Veng;

    InspectorPanel::InspectorPanel(AssetManager& assets, EditorRegistry& editors,
                                   const AssetSourceIndex& sources) :
        m_Assets(assets), m_Editors(editors), m_Sources(sources)
    {
    }

    void InspectorPanel::OnImGui()
    {
        if (m_Scene == nullptr || !m_Selected || !m_Scene->IsAlive(*m_Selected))
        {
            UI::TextDisabled("Nothing selected");
            return;
        }

        TypeRegistry& types = m_Scene->GetTypeRegistry();
        const Entity entity = *m_Selected;

        m_Scene->ForEachComponent(entity, [&](TypeId id, void* component)
        {
            const TypeInfo& info = types.Info(id);

            // A stable per-type id keeps headers from collapsing into one another
            // when two components share a display name.
            auto scope = UI::PushId(fmt::format("{}", id));
            if (UI::CollapsingHeader(info.Name, UI::TreeFlags::DefaultOpen))
                DrawFields(component, info);
        });
    }

    void InspectorPanel::DrawFields(void* base, const TypeInfo& type)
    {
        const FieldWidgetContext ctx{.Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        for (const FieldDescriptor& field : type.Fields)
        {
            if (field.Hidden)
                continue;

            void* fieldPtr = static_cast<u8*>(base) + field.Offset;
            DrawFieldWidget(fieldPtr, field, ctx);
        }
    }
}
