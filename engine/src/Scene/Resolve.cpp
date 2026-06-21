#include <Veng/Scene/Resolve.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
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

    optional<MeshData> BuildShapeMeshData(const PrimitiveShapeVariant& shape)
    {
        const TypeId kind = shape.ActiveType();
        const void* member = shape.ActivePtr();
        if (kind == InvalidTypeId || member == nullptr)
        {
            return std::nullopt;
        }

        if (kind == TypeIdOf<CubeShape>())
        {
            const auto& cube = *static_cast<const CubeShape*>(member);
            return Primitives::Cube(cube.Extent, cube.Material);
        }
        if (kind == TypeIdOf<PlaneShape>())
        {
            const auto& plane = *static_cast<const PlaneShape*>(member);
            return Primitives::Plane(plane.Size, plane.Subdivisions, plane.Material);
        }
        if (kind == TypeIdOf<SphereShape>())
        {
            const auto& sphere = *static_cast<const SphereShape*>(member);
            return Primitives::Sphere(sphere.Radius, sphere.Rings, sphere.Segments,
                                      sphere.Material);
        }
        if (kind == TypeIdOf<IcosphereShape>())
        {
            const auto& ico = *static_cast<const IcosphereShape*>(member);
            return Primitives::Icosphere(ico.Radius, ico.Subdivisions, ico.Material);
        }
        if (kind == TypeIdOf<CylinderShape>())
        {
            const auto& cylinder = *static_cast<const CylinderShape*>(member);
            return Primitives::Cylinder(cylinder.Radius, cylinder.Height, cylinder.Segments,
                                        cylinder.Material);
        }
        if (kind == TypeIdOf<ConeShape>())
        {
            const auto& cone = *static_cast<const ConeShape*>(member);
            return Primitives::Cone(cone.Radius, cone.Height, cone.Segments, cone.Material);
        }
        if (kind == TypeIdOf<TorusShape>())
        {
            const auto& torus = *static_cast<const TorusShape*>(member);
            return Primitives::Torus(torus.MajorRadius, torus.MinorRadius, torus.MajorSegments,
                                     torus.MinorSegments, torus.Material);
        }
        if (kind == TypeIdOf<CapsuleShape>())
        {
            const auto& capsule = *static_cast<const CapsuleShape*>(member);
            return Primitives::Capsule(capsule.Radius, capsule.Height, capsule.Segments,
                                       capsule.Rings, capsule.Material);
        }

        return std::nullopt;
    }

    AssetHandle<Mesh> CreatePrimitiveMesh(AssetManager& manager, const PrimitiveShapeVariant& shape)
    {
        optional<MeshData> data = BuildShapeMeshData(shape);
        if (!data)
        {
            return {};
        }

        const string name = fmt::format("Primitive {:#018x}", shape.ActiveType());
        return manager.CreateAsync<Mesh>(
            Mesh::CreateAsync(manager.GetContext(), manager.GetTasks(), std::move(*data), name));
    }

    void ResolvePrimitive(Primitive& primitive, Scene& scene, Entity entity, AssetManager& manager)
    {
        // An empty variant has no shape to build; leave the renderer untouched.
        if (primitive.Shape.ActiveType() == InvalidTypeId)
        {
            return;
        }

        AssetHandle<Mesh> mesh = CreatePrimitiveMesh(manager, primitive.Shape);
        if (scene.TryGet<MeshRenderer>(entity) == nullptr)
        {
            scene.Add<MeshRenderer>(entity);
        }
        scene.Get<MeshRenderer>(entity).Mesh = std::move(mesh);
    }
}
