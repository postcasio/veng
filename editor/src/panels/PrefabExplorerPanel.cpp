#include "panels/PrefabExplorerPanel.h"

#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>

#include <cctype>
#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        /// @brief Returns @p text lowercased, for case-insensitive substring matching.
        string ToLower(string_view text)
        {
            string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return out;
        }

        /// @brief Returns true if @p ancestorCandidate is @p node or one of its ancestors.
        ///
        /// Walks the Parent edge upward from @p node. Used to reject a drop that would
        /// make an entity its own descendant before it reaches SetParent/MoveBefore,
        /// which fatally assert on a cycle.
        bool IsDescendant(const Scene& scene, Entity ancestorCandidate, Entity node)
        {
            Entity current = node;
            while (!current.IsNull())
            {
                if (current == ancestorCandidate)
                {
                    return true;
                }
                current = scene.GetParent(current);
            }
            return false;
        }

        /// @brief Reads an Entity from a drag-drop payload pointer.
        Entity ReadEntityPayload(const void* payload)
        {
            Entity entity = Entity::Null;
            std::memcpy(&entity, payload, sizeof(Entity));
            return entity;
        }
    }

    PrefabExplorerPanel::PrefabExplorerPanel(PrefabEditContext& ctx) : m_Ctx(ctx) {}

    void PrefabExplorerPanel::OnImGui()
    {
        const Scene* scene = m_Ctx.Scene;
        if (scene == nullptr)
        {
            UI::TextDisabled("No scene");
            return;
        }

        // A structural edit can leave a stale handle in the selection or the rename
        // target; drop dead handles before any accessor touches them.
        m_Ctx.Prune();
        if (!m_Renaming.IsNull() && !scene->IsAlive(m_Renaming))
        {
            m_Renaming = Entity::Null;
        }

        m_Pending.clear();

        // Toolbar: add a root entity, then the name filter.
        if (UI::Button("Add Entity"))
        {
            m_Pending.push_back(PendingOp{.Op = PendingOp::Kind::CreateRoot});
        }
        UI::SameLine();
        UI::SetNextItemWidth(-1.0f);
        if (UI::InputTextWithHint("##search", "Search…", m_Filter))
        {
            m_FilterLower = ToLower(m_Filter);
        }

        BuildSnapshot();

        for (const Entity root : m_Roots)
        {
            DrawEntity(root);
        }

        // A full-width drop zone past the last root reparents a dropped entity to the
        // root; the empty-space context menu adds a root entity.
        UI::Dummy(vec2(UI::ContentRegionAvail().x, 12.0f));
        if (auto target = UI::DragDropTarget())
        {
            if (const void* payload = UI::AcceptDragDropPayload(PrefabEditContext::EntityPayload))
            {
                const Entity dropped = ReadEntityPayload(payload);
                if (scene->IsAlive(dropped) && !scene->GetParent(dropped).IsNull())
                {
                    m_Pending.push_back(
                        PendingOp{.Op = PendingOp::Kind::DetachToRoot, .Source = dropped});
                }
            }
        }

        if (auto menu = UI::PopupContextWindow("hierarchy_empty_menu"))
        {
            if (UI::MenuItem("Add Entity"))
            {
                m_Pending.push_back(PendingOp{.Op = PendingOp::Kind::CreateRoot});
            }
        }

        // The Delete shortcut acts on the selection while the panel is focused.
        if (m_Ctx.HasSelection() && m_Renaming.IsNull() && UI::IsKeyPressed(UI::Key::Delete))
        {
            m_Pending.push_back(PendingOp{.Op = PendingOp::Kind::DeleteSelection});
        }

        // Apply every queued op only now the snapshot walk has returned, so no
        // structural change happens mid-iteration.
        for (const PendingOp& op : m_Pending)
        {
            ApplyOp(op);
        }
    }

    void PrefabExplorerPanel::BuildSnapshot()
    {
        Scene* scene = m_Ctx.Scene;

        // Rebuild the parent/child adjacency from the live scene each frame: cheap for
        // editor-scale prefabs and always current after a structural edit.
        m_Roots.clear();
        m_Children.clear();
        scene->ForEachEntity(
            [&](Entity entity)
            {
                const Entity parent = scene->GetParent(entity);
                if (!parent.IsNull() && scene->IsAlive(parent))
                {
                    m_Children[parent].push_back(entity);
                }
                else
                {
                    m_Roots.push_back(entity);
                }
            });
    }

    void PrefabExplorerPanel::DrawEntity(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        // While a filter is active, hide any entity whose subtree carries no match —
        // an ancestor of a match stays visible so the matching node is reachable.
        if (!m_FilterLower.empty() && !SubtreeMatchesFilter(entity))
        {
            return;
        }

        const auto childrenIt = m_Children.find(entity);
        const bool hasChildren = childrenIt != m_Children.end();

        auto idScope = UI::PushId(fmt::format("e{}.{}", entity.Index, entity.Generation));

        if (m_Renaming == entity)
        {
            DrawRenameField(entity);
        }
        else
        {
            UI::TreeFlags flags = UI::TreeFlags::SpanAvailWidth | UI::TreeFlags::OpenOnArrow |
                                  UI::TreeFlags::DefaultOpen | UI::TreeFlags::AllowOverlap;
            if (!hasChildren)
            {
                flags = flags | UI::TreeFlags::Leaf;
            }
            if (m_Ctx.IsSelected(entity))
            {
                flags = flags | UI::TreeFlags::Selected;
            }

            // Keep the tree-node guard alive across the child draw below: it owns the
            // TreePop, which must run after the children, not when the node is queried.
            auto node = UI::TreeNode(LabelOf(entity), flags);
            const bool open = static_cast<bool>(node);

            // Double-click on the label enters rename; a single click (not on the
            // expand arrow) drives selection, Ctrl toggling the entity in the set.
            if (UI::ItemHovered() && UI::IsMouseDoubleClicked(UI::MouseButton::Left))
            {
                m_Renaming = entity;
                const Name* name = scene->TryGet<Name>(entity);
                m_RenameScratch = name != nullptr ? name->Value : string{};
            }
            else if (UI::IsItemClicked() && !UI::IsItemToggledOpen())
            {
                if (UI::IsCtrlDown())
                {
                    m_Ctx.Toggle(entity);
                }
                else
                {
                    m_Ctx.SelectOnly(entity);
                }
            }

            if (auto source = UI::DragDropSource())
            {
                UI::SetDragDropPayload(PrefabEditContext::EntityPayload, &entity, sizeof(Entity));
                UI::Text(LabelOf(entity));
            }

            if (auto target = UI::DragDropTarget())
            {
                if (const void* payload =
                        UI::AcceptDragDropPayload(PrefabEditContext::EntityPayload))
                {
                    const Entity dropped = ReadEntityPayload(payload);
                    if (scene->IsAlive(dropped) && dropped != entity &&
                        !IsDescendant(*scene, dropped, entity))
                    {
                        m_Pending.push_back(PendingOp{
                            .Op = PendingOp::Kind::Reparent, .Source = dropped, .Target = entity});
                    }
                }
            }

            if (auto menu = UI::PopupContextItem("entity_menu"))
            {
                // Right-clicking a row that is not selected makes it the sole selection,
                // so the menu's selection-wide actions act on what the user clicked.
                if (!m_Ctx.IsSelected(entity))
                {
                    m_Ctx.SelectOnly(entity);
                }
                if (UI::MenuItem("Rename"))
                {
                    m_Renaming = entity;
                    const Name* name = scene->TryGet<Name>(entity);
                    m_RenameScratch = name != nullptr ? name->Value : string{};
                }
                if (UI::MenuItem("Add Child Entity"))
                {
                    m_Pending.push_back(
                        PendingOp{.Op = PendingOp::Kind::CreateChild, .Target = entity});
                }
                if (UI::MenuItem("Duplicate"))
                {
                    m_Pending.push_back(
                        PendingOp{.Op = PendingOp::Kind::Duplicate, .Source = entity});
                }
                if (UI::MenuItem("Delete"))
                {
                    m_Pending.push_back(PendingOp{.Op = PendingOp::Kind::DeleteSelection});
                }
            }

            if (open && hasChildren)
            {
                const u32 depth = entity.Index;
                for (const Entity child : childrenIt->second)
                {
                    DrawReorderTarget(child, depth);
                    DrawEntity(child);
                }
            }
        }
    }

    void PrefabExplorerPanel::DrawReorderTarget(Entity before, u32 depth)
    {
        const Scene* scene = m_Ctx.Scene;

        auto idScope =
            UI::PushId(fmt::format("reorder{}.{}.{}", before.Index, before.Generation, depth));
        UI::Dummy(vec2(UI::ContentRegionAvail().x, 3.0f));
        if (auto target = UI::DragDropTarget())
        {
            if (const void* payload = UI::AcceptDragDropPayload(PrefabEditContext::EntityPayload))
            {
                const Entity dropped = ReadEntityPayload(payload);
                if (scene->IsAlive(dropped) && dropped != before &&
                    !IsDescendant(*scene, dropped, before))
                {
                    m_Pending.push_back(PendingOp{
                        .Op = PendingOp::Kind::MoveBefore, .Source = dropped, .Target = before});
                }
            }
        }
    }

    void PrefabExplorerPanel::DrawRenameField(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        UI::SetNextItemWidth(-1.0f);
        const bool committed = UI::InputTextWithHint("##rename", "Name", m_RenameScratch);
        if (committed)
        {
            // Empty input cancels the rename rather than clearing the name.
            if (!m_RenameScratch.empty())
            {
                if (Name* name = scene->TryGet<Name>(entity))
                {
                    name->Value = m_RenameScratch;
                }
                else
                {
                    scene->Add<Name>(entity, Name{.Value = m_RenameScratch});
                }
            }
            m_Renaming = Entity::Null;
        }
    }

    void PrefabExplorerPanel::ApplyOp(const PendingOp& op)
    {
        Scene* scene = m_Ctx.Scene;

        switch (op.Op)
        {
        case PendingOp::Kind::CreateRoot:
        {
            const Entity created = scene->CreateEntity();
            scene->Add<Name>(created, Name{.Value = "Entity"});
            scene->Add<Transform>(created);
            m_Ctx.SelectOnly(created);
            break;
        }
        case PendingOp::Kind::CreateChild:
        {
            if (!scene->IsAlive(op.Target))
            {
                break;
            }
            const Entity created = scene->CreateEntity();
            scene->Add<Name>(created, Name{.Value = "Entity"});
            scene->Add<Transform>(created);
            scene->SetParent(created, op.Target);
            m_Ctx.SelectOnly(created);
            break;
        }
        case PendingOp::Kind::Reparent:
        {
            if (scene->IsAlive(op.Source) && scene->IsAlive(op.Target) && op.Source != op.Target &&
                !IsDescendant(*scene, op.Source, op.Target))
            {
                scene->SetParent(op.Source, op.Target);
            }
            break;
        }
        case PendingOp::Kind::MoveBefore:
        {
            if (scene->IsAlive(op.Source) && scene->IsAlive(op.Target) && op.Source != op.Target &&
                !IsDescendant(*scene, op.Source, op.Target))
            {
                scene->MoveBefore(op.Source, op.Target);
            }
            break;
        }
        case PendingOp::Kind::DetachToRoot:
        {
            if (scene->IsAlive(op.Source))
            {
                scene->Detach(op.Source);
            }
            break;
        }
        case PendingOp::Kind::Duplicate:
        {
            if (scene->IsAlive(op.Source))
            {
                const Entity parent = scene->GetParent(op.Source);
                const Entity copy = DuplicateSubtree(op.Source, parent);
                m_Ctx.SelectOnly(copy);
            }
            break;
        }
        case PendingOp::Kind::DeleteSelection:
        {
            // Copy out the selection: DestroyEntity is recursive and prunes the set.
            const vector<Entity> targets = m_Ctx.Selection;
            for (const Entity entity : targets)
            {
                if (scene->IsAlive(entity))
                {
                    scene->DestroyEntity(entity);
                }
            }
            m_Ctx.Prune();
            break;
        }
        }
    }

    Entity PrefabExplorerPanel::DuplicateSubtree(Entity source, Entity newParent)
    {
        Scene* scene = m_Ctx.Scene;
        TypeRegistry& types = scene->GetTypeRegistry();
        const TypeId hierarchyId = types.IdOf<Hierarchy>();

        const Entity copy = scene->CreateEntity();

        // Copy every component except Hierarchy by round-tripping its fields through the
        // reflection serializer. Hierarchy is excluded — its links are rebuilt via
        // SetParent below. Cross-entity Reference fields are NOT remapped here; only the
        // parent/child hierarchy links are rebuilt for the copied subtree.
        vector<u8> scratch;
        scene->ForEachComponent(
            source,
            [&](TypeId id, void* component)
            {
                if (id == hierarchyId)
                {
                    return;
                }
                const TypeInfo& info = types.Info(id);
                scratch.clear();
                WriteFields(scratch, component, info, types);

                void* slot = scene->AddComponent(copy, id);
                const VoidResult read = ReadFields(scratch, slot, info, types);
                VE_ASSERT(read.has_value(), "Duplicate: ReadFields failed for '{}': {}", info.Name,
                          read.has_value() ? string{} : read.error());
            });

        if (!newParent.IsNull() && scene->IsAlive(newParent))
        {
            scene->SetParent(copy, newParent);
        }

        // Snapshot the source's children before recursing: DuplicateSubtree adds entities,
        // so the live child walk cannot run concurrently with the structural edits.
        vector<Entity> sourceChildren;
        scene->ForEachChild(source, [&](Entity child) { sourceChildren.push_back(child); });
        for (const Entity child : sourceChildren)
        {
            DuplicateSubtree(child, copy);
        }

        return copy;
    }

    bool PrefabExplorerPanel::MatchesFilter(string_view name) const
    {
        if (m_FilterLower.empty())
        {
            return true;
        }
        const string lowered = ToLower(name);
        return lowered.find(m_FilterLower) != string::npos;
    }

    bool PrefabExplorerPanel::SubtreeMatchesFilter(Entity entity) const
    {
        if (MatchesFilter(LabelOf(entity)))
        {
            return true;
        }
        const auto childrenIt = m_Children.find(entity);
        if (childrenIt != m_Children.end())
        {
            for (const Entity child : childrenIt->second)
            {
                if (SubtreeMatchesFilter(child))
                {
                    return true;
                }
            }
        }
        return false;
    }

    string PrefabExplorerPanel::LabelOf(Entity entity) const
    {
        const Name* name = m_Ctx.Scene->TryGet<Name>(entity);
        if (name != nullptr && !name->Value.empty())
        {
            return name->Value;
        }
        return fmt::format("Entity {}", entity.Index);
    }
}
