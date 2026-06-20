#include "panels/PrefabExplorerPanel.h"

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    PrefabExplorerPanel::PrefabExplorerPanel(PrefabEditContext& ctx) : m_Ctx(ctx) {}

    void PrefabExplorerPanel::OnImGui()
    {
        Scene* scene = m_Ctx.Scene;
        if (scene == nullptr)
        {
            UI::TextDisabled("No scene");
            return;
        }

        // Rebuild the parent/child adjacency from the live scene each frame: cheap for
        // editor-scale prefabs and always current after a structural edit.
        m_Roots.clear();
        m_Children.clear();
        scene->ForEachEntity(
            [&](Entity entity)
            {
                const Parent* parent = scene->TryGet<Parent>(entity);
                if (parent != nullptr && !parent->Value.IsNull() && scene->IsAlive(parent->Value))
                {
                    m_Children[parent->Value].push_back(entity);
                }
                else
                {
                    m_Roots.push_back(entity);
                }
            });

        for (const Entity root : m_Roots)
        {
            DrawEntity(root);
        }
    }

    void PrefabExplorerPanel::DrawEntity(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        const Name* name = scene->TryGet<Name>(entity);
        const string label = name != nullptr && !name->Value.empty()
                                 ? name->Value
                                 : fmt::format("Entity {}", entity.Index);

        const auto childrenIt = m_Children.find(entity);
        const bool hasChildren = childrenIt != m_Children.end();

        UI::TreeFlags flags =
            UI::TreeFlags::SpanAvailWidth | UI::TreeFlags::OpenOnArrow | UI::TreeFlags::DefaultOpen;
        if (!hasChildren)
        {
            flags = flags | UI::TreeFlags::Leaf;
        }
        if (m_Ctx.Selection == entity)
        {
            flags = flags | UI::TreeFlags::Selected;
        }

        auto scope = UI::PushId(fmt::format("e{}", entity.Index));
        const auto node = UI::TreeNode(label, flags);
        if (UI::IsItemClicked())
        {
            m_Ctx.Selection = entity;
        }

        if (node && hasChildren)
        {
            for (const Entity child : childrenIt->second)
            {
                DrawEntity(child);
            }
        }
    }
}
