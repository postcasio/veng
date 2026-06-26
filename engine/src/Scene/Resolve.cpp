#include <Veng/Scene/Resolve.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>

namespace Veng
{
    optional<MeshData> BuildShapeMeshData(const MeshSource& source)
    {
        const TypeId kind = source.ActiveType();
        const void* member = source.ActivePtr();
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

    AssetHandle<Mesh> BuildPrimitiveMesh(AssetManager& manager, const MeshSource& source)
    {
        optional<MeshData> data = BuildShapeMeshData(source);
        if (!data)
        {
            return {};
        }

        const string name = fmt::format("Primitive {:#018x}", source.ActiveType());
        return manager.Build<Mesh>(std::move(*data), name);
    }
}
