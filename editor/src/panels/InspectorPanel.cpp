#include "panels/InspectorPanel.h"

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
                                   const AssetSourceIndex& sources, PrefabEditContext& ctx)
        : m_Assets(assets), m_Editors(editors), m_Sources(sources), m_Ctx(ctx)
    {
    }

    void InspectorPanel::OnImGui()
    {
        Scene* scene = m_Ctx.Scene;
        if (scene == nullptr || !m_Ctx.Selection || !scene->IsAlive(*m_Ctx.Selection))
        {
            UI::TextDisabled("Nothing selected");
            return;
        }

        TypeRegistry& types = scene->GetTypeRegistry();
        const Entity entity = *m_Ctx.Selection;

        scene->ForEachComponent(entity,
                                [&](TypeId id, void* component)
                                {
                                    const TypeInfo& info = types.Info(id);

                                    // A stable per-type id keeps headers from collapsing into one another
                                    // when two components share a display name.
                                    auto scope = UI::PushId(fmt::format("{}", id));
                                    if (UI::CollapsingHeader(info.Name, UI::TreeFlags::DefaultOpen))
                                    {
                                        DrawFields(component, info);
                                    }
                                });
    }

    void InspectorPanel::DrawFields(void* base, const TypeInfo& type)
    {
        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        for (const FieldDescriptor& field : type.Fields)
        {
            if (field.Hidden)
            {
                continue;
            }

            void* fieldPtr = static_cast<u8*>(base) + field.Offset;
            DrawFieldWidget(fieldPtr, field, ctx);
        }
    }
}
