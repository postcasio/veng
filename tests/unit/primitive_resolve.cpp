// BuildShapeMeshData unit cases: the active alternative of a PrimitiveShapeVariant
// dispatches to the matching Primitives:: generator, producing the same geometry;
// an empty variant yields nullopt. Pure CPU — no Context, no Vulkan.

#include <doctest/doctest.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Resolve.h>

using namespace Veng;

namespace
{
    template <class Shape>
    PrimitiveShapeVariant MakeVariant(const Shape& value)
    {
        PrimitiveShapeVariant variant;
        *static_cast<Shape*>(variant.SetActive(TypeIdOf<Shape>())) = value;
        return variant;
    }
}

TEST_CASE("BuildShapeMeshData on an empty variant returns nullopt")
{
    const PrimitiveShapeVariant empty;
    CHECK_FALSE(BuildShapeMeshData(empty).has_value());
}

TEST_CASE("BuildShapeMeshData(Cube) matches Primitives::Cube")
{
    const PrimitiveShapeVariant variant = MakeVariant(CubeShape{.Extent = 2.0f});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Cube(2.0f);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Plane) matches Primitives::Plane")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(PlaneShape{.Size = vec2(3.0f), .Subdivisions = uvec2(2)});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Plane(vec2(3.0f), uvec2(2));
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Sphere) matches Primitives::Sphere")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(SphereShape{.Radius = 1.0f, .Rings = 8, .Segments = 12});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Sphere(1.0f, 8, 12);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Icosphere) matches Primitives::Icosphere")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(IcosphereShape{.Radius = 0.7f, .Subdivisions = 2});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Icosphere(0.7f, 2);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Cylinder) matches Primitives::Cylinder")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(CylinderShape{.Radius = 0.6f, .Height = 1.5f, .Segments = 20});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Cylinder(0.6f, 1.5f, 20);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Cone) matches Primitives::Cone")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(ConeShape{.Radius = 0.7f, .Height = 1.2f, .Segments = 18});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Cone(0.7f, 1.2f, 18);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Torus) matches Primitives::Torus")
{
    const PrimitiveShapeVariant variant = MakeVariant(TorusShape{
        .MajorRadius = 0.8f, .MinorRadius = 0.25f, .MajorSegments = 20, .MinorSegments = 10});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Torus(0.8f, 0.25f, 20, 10);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("BuildShapeMeshData(Capsule) matches Primitives::Capsule")
{
    const PrimitiveShapeVariant variant =
        MakeVariant(CapsuleShape{.Radius = 0.4f, .Height = 1.0f, .Segments = 16, .Rings = 6});
    const optional<MeshData> built = BuildShapeMeshData(variant);
    REQUIRE(built.has_value());

    const MeshData expected = Primitives::Capsule(0.4f, 1.0f, 16, 6);
    CHECK(built->Vertices.size() == expected.Vertices.size());
    CHECK(built->Indices.size() == expected.Indices.size());
}

TEST_CASE("PrimitiveComponent round-trips its active shape through reflection")
{
    TypeRegistry registry;
    registry.Register<PrimitiveComponent>();
    const TypeInfo& info = registry.Info(TypeIdOf<PrimitiveComponent>());

    PrimitiveComponent src;
    *static_cast<SphereShape*>(src.Shape.SetActive(TypeIdOf<SphereShape>())) =
        SphereShape{.Radius = 1.5f, .Rings = 10, .Segments = 20};

    vector<u8> bytes;
    WriteFields(bytes, &src, info, registry);

    PrimitiveComponent dst;
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    REQUIRE(dst.Shape.ActiveType() == TypeIdOf<SphereShape>());
    const auto& shape = *static_cast<const SphereShape*>(dst.Shape.ActivePtr());
    CHECK(shape.Radius == doctest::Approx(1.5f));
    CHECK(shape.Rings == 10);
    CHECK(shape.Segments == 20);

    // Both produce an equivalent mesh after resolution.
    const optional<MeshData> a = BuildShapeMeshData(src.Shape);
    const optional<MeshData> b = BuildShapeMeshData(dst.Shape);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->Indices.size() == b->Indices.size());
}
