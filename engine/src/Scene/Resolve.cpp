#include <Veng/Scene/Resolve.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void ResolveComponents(Scene& scene, Entity entity, AssetManager& manager)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();

        // Collect the resolver-bearing component types first, then fire outside the
        // walk: a resolver may Add a component, which is a structural change illegal
        // mid-iteration.
        vector<TypeId> resolvers;
        scene.ForEachComponent(entity,
                               [&](TypeId id, void*)
                               {
                                   if (registry.Info(id).SpawnResolve != nullptr)
                                   {
                                       resolvers.push_back(id);
                                   }
                               });

        for (const TypeId id : resolvers)
        {
            // Fetch the storage fresh by TypeId: a prior resolver's Add may have grown
            // a pool, dangling any pointer held across it.
            void* slot = scene.TryGetComponent(entity, id);
            if (slot != nullptr)
            {
                registry.Info(id).SpawnResolve(slot, scene, entity, manager);
            }
        }
    }
}
