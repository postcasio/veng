#include "EditorCommand.h"

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Re-resolve a restored component's AssetHandle and Reference fields through the loader
        // path — the same RemapComponentReferences pass Prefab::SpawnInto and Scene::Clone use,
        // never a raw memcpy. A Reference field holds a live Entity handle (the snapshot captured
        // it live), so the remap is identity: the reference keeps pointing at the same entity. An
        // AssetHandle field carries only its AssetId after ReadFields; rehydrate it to the live
        // cache entry so a stale handle is never reinstated. A runtime-adopted (id-less) handle —
        // a recipe-built mesh — round-trips to empty here and is rebuilt by ResolveEntity instead.
        void ResolveRestoredComponent(void* component, const TypeInfo& info,
                                      const TypeRegistry& types, AssetManager& assets)
        {
            const EntityRemap identity = [](Entity reference) { return reference; };
            const AssetHandleFixup rehydrate = [&assets](void* fieldPtr)
            {
                AssetId id{};
                std::memcpy(&id, fieldPtr, sizeof(id));
                if (id.IsValid())
                {
                    const Ref<Detail::AssetCacheEntry> entry = assets.CachedEntry(id);
                    if (entry != nullptr)
                    {
                        Detail::RehydrateHandleField(fieldPtr, id, entry);
                    }
                }
            };
            RemapComponentReferences(component, info, types, identity, rehydrate);
        }

        // Deserialize component bytes into a live component, then re-resolve its handle/reference
        // fields. The component must already exist on the entity (added by the caller). Re-runs
        // ResolveEntity so a touched MeshRenderer re-streams its derived mesh.
        void RestoreComponentBytes(PrefabEditContext& ctx, Entity entity, TypeId typeId,
                                   const vector<u8>& bytes)
        {
            Scene* scene = ctx.Scene;
            if (scene == nullptr || !scene->IsAlive(entity))
            {
                return;
            }
            void* component = scene->TryGetComponent(entity, typeId);
            if (component == nullptr)
            {
                return;
            }
            const TypeRegistry& types = scene->GetTypeRegistry();
            const TypeInfo& info = types.Info(typeId);

            const VoidResult read = ReadFields(bytes, component, info, types);
            VE_ASSERT(read.has_value(), "EditorCommand: ReadFields failed for '{}': {}", info.Name,
                      read.has_value() ? string{} : read.error());

            if (ctx.Assets != nullptr)
            {
                ResolveRestoredComponent(component, info, types, *ctx.Assets);
            }
            ctx.ResolveEntity(entity);
        }

        // The entity the moved child sat before in its sibling list (its NextSibling), or Null when
        // it was the last/only child or root. Restoring this exact neighbor with MoveBefore puts the
        // child back in its original ordered slot; Null means it was last, restored by appending.
        Entity NextSiblingOf(const Scene& scene, Entity entity)
        {
            const Entity parent = scene.GetParent(entity);
            Entity found = Entity::Null;
            if (parent.IsNull())
            {
                // Roots have no parent child-list; their order is slot order (ForEachEntity).
                bool seen = false;
                scene.ForEachEntity(
                    [&](Entity candidate)
                    {
                        if (found != Entity::Null)
                        {
                            return;
                        }
                        if (seen && scene.GetParent(candidate).IsNull())
                        {
                            found = candidate;
                        }
                        if (candidate == entity)
                        {
                            seen = true;
                        }
                    });
                return found;
            }
            bool seen = false;
            scene.ForEachChild(parent,
                               [&](Entity child)
                               {
                                   if (found != Entity::Null)
                                   {
                                       return;
                                   }
                                   if (seen)
                                   {
                                       found = child;
                                   }
                                   if (child == entity)
                                   {
                                       seen = true;
                                   }
                               });
            return found;
        }

        // Snapshot a subtree depth-first (parent before children), recording each entity's exact
        // handle, parent, and every non-Hierarchy component's bytes. The order lets a respawn
        // recreate parents before children so SetParent always finds a live parent.
        void SnapshotSubtree(Scene& scene, Entity root, vector<EntitySnapshot>& out)
        {
            const TypeRegistry& types = scene.GetTypeRegistry();
            const TypeId hierarchyId = TypeIdOf<Hierarchy>();

            EntitySnapshot snapshot;
            snapshot.Handle = root;
            snapshot.Parent = scene.GetParent(root);

            vector<u8> scratch;
            scene.ForEachComponent(root,
                                   [&](TypeId id, void* component)
                                   {
                                       if (id == hierarchyId)
                                       {
                                           return;
                                       }
                                       const TypeInfo& info = types.Info(id);
                                       scratch.clear();
                                       WriteFields(scratch, component, info, types);
                                       snapshot.Components.push_back(
                                           {.Type = id, .Bytes = scratch});
                                   });
            out.push_back(std::move(snapshot));

            // Snapshot children after the parent is recorded; ForEachComponent has returned, so the
            // child walk runs no structural change mid-iteration.
            vector<Entity> children;
            scene.ForEachChild(root, [&](Entity child) { children.push_back(child); });
            for (const Entity child : children)
            {
                SnapshotSubtree(scene, child, out);
            }
        }

        // Respawn a snapshotted subtree at its exact handles (CreateEntityAt), restore each
        // component through the loader path, rebuild the hierarchy links, and re-resolve mesh
        // sources. The snapshot is in parent-before-child order, so a parent is live by the time a
        // child reparents to it.
        void RespawnSubtree(PrefabEditContext& ctx, const vector<EntitySnapshot>& snapshot)
        {
            Scene* scene = ctx.Scene;
            if (scene == nullptr)
            {
                return;
            }
            const TypeRegistry& types = scene->GetTypeRegistry();

            for (const EntitySnapshot& entry : snapshot)
            {
                scene->CreateEntityAt(entry.Handle);
                for (const EntitySnapshot::Component& component : entry.Components)
                {
                    scene->AddComponent(entry.Handle, component.Type);
                }
            }

            // Restore components and links in a second pass so a Reference field pointing forward in
            // the subtree lands on an already-created handle.
            for (const EntitySnapshot& entry : snapshot)
            {
                if (!entry.Parent.IsNull() && scene->IsAlive(entry.Parent))
                {
                    scene->SetParent(entry.Handle, entry.Parent);
                }
                for (const EntitySnapshot::Component& component : entry.Components)
                {
                    void* slot = scene->TryGetComponent(entry.Handle, component.Type);
                    if (slot == nullptr)
                    {
                        continue;
                    }
                    const TypeInfo& info = types.Info(component.Type);
                    const VoidResult read = ReadFields(component.Bytes, slot, info, types);
                    VE_ASSERT(read.has_value(), "EditorCommand: respawn ReadFields failed for '{}'",
                              info.Name);
                    if (ctx.Assets != nullptr)
                    {
                        ResolveRestoredComponent(slot, info, types, *ctx.Assets);
                    }
                }
                ctx.ResolveEntity(entry.Handle);
            }
        }
    }

    // --- EditTransform ----------------------------------------------------------

    EditTransform::EditTransform(Entity entity, const Transform& before, const Transform& after)
        : m_Entity(entity), m_Before(before), m_After(after)
    {
    }

    void EditTransform::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && scene->IsAlive(m_Entity) &&
            scene->TryGet<Transform>(m_Entity) != nullptr)
        {
            scene->Get<Transform>(m_Entity) = m_After;
        }
    }

    void EditTransform::Revert(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && scene->IsAlive(m_Entity) &&
            scene->TryGet<Transform>(m_Entity) != nullptr)
        {
            scene->Get<Transform>(m_Entity) = m_Before;
        }
    }

    // --- EditField --------------------------------------------------------------

    EditField::EditField(Entity entity, TypeId typeId, vector<u8> before, vector<u8> after)
        : m_Entity(entity), m_TypeId(typeId), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    void EditField::Restore(PrefabEditContext& ctx, const vector<u8>& bytes) const
    {
        RestoreComponentBytes(ctx, m_Entity, m_TypeId, bytes);
    }

    void EditField::Apply(PrefabEditContext& ctx)
    {
        Restore(ctx, m_After);
    }

    void EditField::Revert(PrefabEditContext& ctx)
    {
        Restore(ctx, m_Before);
    }

    // --- AddComponentCommand ----------------------------------------------------

    AddComponentCommand::AddComponentCommand(Entity entity, TypeId typeId)
        : m_Entity(entity), m_TypeId(typeId)
    {
    }

    void AddComponentCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && scene->IsAlive(m_Entity))
        {
            scene->AddComponent(m_Entity, m_TypeId);
            ctx.ResolveEntity(m_Entity);
        }
    }

    void AddComponentCommand::Revert(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && scene->IsAlive(m_Entity))
        {
            scene->RemoveComponent(m_Entity, m_TypeId);
        }
    }

    // --- RemoveComponentCommand -------------------------------------------------

    RemoveComponentCommand::RemoveComponentCommand(Entity entity, TypeId typeId,
                                                   vector<u8> snapshot)
        : m_Entity(entity), m_TypeId(typeId), m_Snapshot(std::move(snapshot))
    {
    }

    void RemoveComponentCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && scene->IsAlive(m_Entity))
        {
            scene->RemoveComponent(m_Entity, m_TypeId);
        }
    }

    void RemoveComponentCommand::Revert(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr || !scene->IsAlive(m_Entity))
        {
            return;
        }
        scene->AddComponent(m_Entity, m_TypeId);
        RestoreComponentBytes(ctx, m_Entity, m_TypeId, m_Snapshot);
    }

    // --- ResetComponentCommand --------------------------------------------------

    ResetComponentCommand::ResetComponentCommand(Entity entity, TypeId typeId, vector<u8> snapshot)
        : m_Entity(entity), m_TypeId(typeId), m_Snapshot(std::move(snapshot))
    {
    }

    void ResetComponentCommand::Restore(PrefabEditContext& ctx, const vector<u8>& bytes) const
    {
        RestoreComponentBytes(ctx, m_Entity, m_TypeId, bytes);
    }

    void ResetComponentCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr || !scene->IsAlive(m_Entity))
        {
            return;
        }
        void* component = scene->TryGetComponent(m_Entity, m_TypeId);
        if (component == nullptr)
        {
            return;
        }
        const TypeInfo& info = scene->GetTypeRegistry().Info(m_TypeId);
        info.Destruct(component);
        info.DefaultConstruct(component);
        ctx.ResolveEntity(m_Entity);
    }

    void ResetComponentCommand::Revert(PrefabEditContext& ctx)
    {
        Restore(ctx, m_Snapshot);
    }

    // --- ReparentCommand --------------------------------------------------------

    ReparentCommand::ReparentCommand(Entity entity, Entity oldParent, Entity oldNextSibling,
                                     Entity newParent, Entity newNextSibling)
        : m_Entity(entity), m_OldParent(oldParent), m_OldNextSibling(oldNextSibling),
          m_NewParent(newParent), m_NewNextSibling(newNextSibling)
    {
    }

    void ReparentCommand::MoveTo(PrefabEditContext& ctx, Entity parent, Entity nextSibling) const
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr || !scene->IsAlive(m_Entity))
        {
            return;
        }
        // Insert before the captured neighbor when it is still alive; otherwise the entity was
        // last in its list — append under the parent (or detach to root).
        if (!nextSibling.IsNull() && scene->IsAlive(nextSibling))
        {
            scene->MoveBefore(m_Entity, nextSibling);
        }
        else if (!parent.IsNull() && scene->IsAlive(parent))
        {
            scene->SetParent(m_Entity, parent);
        }
        else
        {
            scene->Detach(m_Entity);
        }
    }

    void ReparentCommand::Apply(PrefabEditContext& ctx)
    {
        MoveTo(ctx, m_NewParent, m_NewNextSibling);
    }

    void ReparentCommand::Revert(PrefabEditContext& ctx)
    {
        MoveTo(ctx, m_OldParent, m_OldNextSibling);
    }

    // --- CreateEntityCommand ----------------------------------------------------

    CreateEntityCommand::CreateEntityCommand(Entity parent) : m_Parent(parent) {}

    void CreateEntityCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr)
        {
            return;
        }
        // The first Apply creates a fresh handle; a Redo respawns that exact handle so any later
        // stack capture of it stays valid.
        const Entity created =
            m_Created.IsNull() ? scene->CreateEntity() : scene->CreateEntityAt(m_Created);
        m_Created = created;
        scene->Add<Name>(created, Name{.Value = "Entity"});
        scene->Add<Transform>(created);
        if (!m_Parent.IsNull() && scene->IsAlive(m_Parent))
        {
            scene->SetParent(created, m_Parent);
        }
    }

    void CreateEntityCommand::Revert(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && !m_Created.IsNull() && scene->IsAlive(m_Created))
        {
            scene->DestroyEntity(m_Created);
        }
    }

    // --- DestroyEntityCommand ---------------------------------------------------

    DestroyEntityCommand::DestroyEntityCommand(Entity root) : m_Root(root) {}

    void DestroyEntityCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr || !scene->IsAlive(m_Root))
        {
            return;
        }
        // Capture the subtree on the first Apply only; a Redo destroys the already-captured set.
        if (m_Snapshot.empty())
        {
            SnapshotSubtree(*scene, m_Root, m_Snapshot);
        }
        scene->DestroyEntity(m_Root);
    }

    void DestroyEntityCommand::Revert(PrefabEditContext& ctx)
    {
        RespawnSubtree(ctx, m_Snapshot);
    }

    // --- DuplicateEntityCommand -------------------------------------------------

    DuplicateEntityCommand::DuplicateEntityCommand(Entity source) : m_Source(source) {}

    void DuplicateEntityCommand::Apply(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene == nullptr)
        {
            return;
        }
        // First Apply: actually duplicate the live source and snapshot the resulting copy so a
        // Redo respawns the same copy handles. Subsequent Redo respawns from the snapshot.
        if (m_Captured)
        {
            RespawnSubtree(ctx, m_Snapshot);
            return;
        }
        if (!scene->IsAlive(m_Source))
        {
            return;
        }

        const TypeRegistry& types = scene->GetTypeRegistry();
        const TypeId hierarchyId = TypeIdOf<Hierarchy>();
        const Entity parent = scene->GetParent(m_Source);

        // Recursive copy mirroring the explorer's DuplicateSubtree: round-trip each non-Hierarchy
        // component, rebuild links with SetParent, and re-resolve mesh sources.
        const function<Entity(Entity, Entity)> duplicate = [&](Entity source,
                                                               Entity newParent) -> Entity
        {
            const Entity copy = scene->CreateEntity();
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
                    VE_ASSERT(read.has_value(), "Duplicate: ReadFields failed for '{}'", info.Name);
                });

            if (!newParent.IsNull() && scene->IsAlive(newParent))
            {
                scene->SetParent(copy, newParent);
            }

            vector<Entity> children;
            scene->ForEachChild(source, [&](Entity child) { children.push_back(child); });
            for (const Entity child : children)
            {
                (void)duplicate(child, copy);
            }
            ctx.ResolveEntity(copy);
            return copy;
        };

        m_Copy = duplicate(m_Source, parent);
        SnapshotSubtree(*scene, m_Copy, m_Snapshot);
        m_Captured = true;
    }

    void DuplicateEntityCommand::Revert(PrefabEditContext& ctx)
    {
        Scene* scene = ctx.Scene;
        if (scene != nullptr && !m_Copy.IsNull() && scene->IsAlive(m_Copy))
        {
            scene->DestroyEntity(m_Copy);
        }
    }
}
