// AABB algebra + mesh/scene bounds unit cases: pure CPU math and a device-free
// scene fold. No Context, no Vulkan — a Mesh built through the MeshInfo factory
// carries only a name + bound (empty buffers), enough for SceneBounds to read
// GetBounds() and AssetManager::Adopt to wrap it in a resident handle.

#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Math/AABB.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps &&
               std::abs(a.z - b.z) <= eps;
    }
}

TEST_CASE("AABB::Empty is the identity for Union")
{
    const AABB empty = AABB::Empty();
    CHECK(empty.IsEmpty());

    const AABB box{.Min = vec3(-1.0f, -2.0f, -3.0f), .Max = vec3(4.0f, 5.0f, 6.0f)};
    CHECK_FALSE(box.IsEmpty());

    const AABB unioned = Union(empty, box);
    CHECK(VecApprox(unioned.Min, box.Min));
    CHECK(VecApprox(unioned.Max, box.Max));

    // Symmetric: empty on either side.
    const AABB unioned2 = Union(box, empty);
    CHECK(VecApprox(unioned2.Min, box.Min));
    CHECK(VecApprox(unioned2.Max, box.Max));

    // Union of two empties stays empty.
    CHECK(Union(empty, empty).IsEmpty());
}

TEST_CASE("AABB::Expand grows tight min/max for points and boxes")
{
    AABB box = AABB::Empty();
    box.Expand(vec3(1.0f, 2.0f, 3.0f));
    CHECK(VecApprox(box.Min, vec3(1.0f, 2.0f, 3.0f)));
    CHECK(VecApprox(box.Max, vec3(1.0f, 2.0f, 3.0f)));

    box.Expand(vec3(-1.0f, 5.0f, 0.0f));
    CHECK(VecApprox(box.Min, vec3(-1.0f, 2.0f, 0.0f)));
    CHECK(VecApprox(box.Max, vec3(1.0f, 5.0f, 3.0f)));

    AABB const other{.Min = vec3(-2.0f, 0.0f, -1.0f), .Max = vec3(0.0f, 10.0f, 4.0f)};
    box.Expand(other);
    CHECK(VecApprox(box.Min, vec3(-2.0f, 0.0f, -1.0f)));
    CHECK(VecApprox(box.Max, vec3(1.0f, 10.0f, 4.0f)));
}

TEST_CASE("AABB Center/Extents/Size on a known box")
{
    const AABB box{.Min = vec3(-2.0f, 0.0f, 4.0f), .Max = vec3(2.0f, 6.0f, 10.0f)};
    CHECK(VecApprox(box.Center(), vec3(0.0f, 3.0f, 7.0f)));
    CHECK(VecApprox(box.Extents(), vec3(2.0f, 3.0f, 3.0f)));
    CHECK(VecApprox(box.Size(), vec3(4.0f, 6.0f, 6.0f)));
}

TEST_CASE("AABB::Transformed refits under rotation (corners, not min/max)")
{
    // A unit box rotated 45° about Z. The refit box is larger than the original
    // (the corners swing out), not the same box.
    const AABB unit{.Min = vec3(-0.5f), .Max = vec3(0.5f)};
    const mat4 rot = glm::rotate(mat4(1.0f), glm::radians(45.0f), vec3(0.0f, 0.0f, 1.0f));
    const AABB refit = unit.Transformed(rot);

    // 0.5 unit half-extent → the rotated XY extent is 0.5 * sqrt(2) ≈ 0.7071.
    const f32 expected = 0.5f * std::sqrt(2.0f);
    CHECK(refit.Max.x == doctest::Approx(expected).epsilon(0.001f));
    CHECK(refit.Max.y == doctest::Approx(expected).epsilon(0.001f));
    CHECK(refit.Min.x == doctest::Approx(-expected).epsilon(0.001f));
    CHECK(refit.Min.y == doctest::Approx(-expected).epsilon(0.001f));
    // Z is the rotation axis, unchanged.
    CHECK(refit.Max.z == doctest::Approx(0.5f).epsilon(0.001f));
    CHECK(refit.Min.z == doctest::Approx(-0.5f).epsilon(0.001f));
}

TEST_CASE("AABB::Transformed translates by the matrix")
{
    const AABB unit{.Min = vec3(-0.5f), .Max = vec3(0.5f)};
    const mat4 translate = glm::translate(mat4(1.0f), vec3(10.0f, -3.0f, 2.0f));
    const AABB moved = unit.Transformed(translate);
    CHECK(VecApprox(moved.Center(), vec3(10.0f, -3.0f, 2.0f)));
    CHECK(VecApprox(moved.Size(), vec3(1.0f)));
}

TEST_CASE("Mesh::ComputeBounds: typed and raw forms agree, empty for no vertices")
{
    CHECK(Mesh::ComputeBounds(std::span<const CanonicalVertex>{}).IsEmpty());

    const MeshData cube = Primitives::Cube(2.0f);
    const AABB typed = Mesh::ComputeBounds(std::span<const CanonicalVertex>(cube.Vertices));

    const std::span<const u8> raw(reinterpret_cast<const u8*>(cube.Vertices.data()),
                                  cube.Vertices.size() * sizeof(CanonicalVertex));
    const AABB fromBytes = Mesh::ComputeBounds(raw, sizeof(CanonicalVertex));

    CHECK(VecApprox(typed.Min, fromBytes.Min));
    CHECK(VecApprox(typed.Max, fromBytes.Max));
}

TEST_CASE("Mesh::ComputeSubMeshBounds: folds exactly a referenced index range")
{
    // Four vertices spread on X; two disjoint index ranges referencing the left pair
    // and the right pair. The fold over each range bounds only that pair.
    const vector<CanonicalVertex> vertices = {
        {.Position = vec3(-10.0f, 0.0f, 0.0f)},
        {.Position = vec3(-8.0f, 1.0f, -1.0f)},
        {.Position = vec3(8.0f, -1.0f, 1.0f)},
        {.Position = vec3(10.0f, 0.0f, 0.0f)},
    };
    const vector<u32> indices = {0, 1, 2, 3};

    const std::span<const CanonicalVertex> v(vertices);
    const std::span<const u32> idx(indices);

    // Empty range → Empty().
    CHECK(Mesh::ComputeSubMeshBounds(v, idx, 0, 0).IsEmpty());

    const AABB left = Mesh::ComputeSubMeshBounds(v, idx, 0, 2);
    CHECK(VecApprox(left.Min, vec3(-10.0f, 0.0f, -1.0f)));
    CHECK(VecApprox(left.Max, vec3(-8.0f, 1.0f, 0.0f)));

    const AABB right = Mesh::ComputeSubMeshBounds(v, idx, 2, 2);
    CHECK(VecApprox(right.Min, vec3(8.0f, -1.0f, 0.0f)));
    CHECK(VecApprox(right.Max, vec3(10.0f, 0.0f, 1.0f)));

    // A range covering every index equals Mesh::ComputeBounds over the whole span.
    const AABB whole = Mesh::ComputeSubMeshBounds(v, idx, 0, static_cast<u32>(indices.size()));
    const AABB full = Mesh::ComputeBounds(v);
    CHECK(VecApprox(whole.Min, full.Min));
    CHECK(VecApprox(whole.Max, full.Max));

    // The raw-bytes overload agrees with the typed one over the same range.
    const std::span<const u8> raw(reinterpret_cast<const u8*>(vertices.data()),
                                  vertices.size() * sizeof(CanonicalVertex));
    const AABB leftBytes = Mesh::ComputeSubMeshBounds(raw, sizeof(CanonicalVertex), idx, 0, 2);
    CHECK(VecApprox(leftBytes.Min, left.Min));
    CHECK(VecApprox(leftBytes.Max, left.Max));
    CHECK(Mesh::ComputeSubMeshBounds(raw, sizeof(CanonicalVertex), idx, 0, 0).IsEmpty());
}

TEST_CASE("Primitive bounds: Cube/Sphere/Plane")
{
    const AABB cube =
        Mesh::ComputeBounds(std::span<const CanonicalVertex>(Primitives::Cube(1.0f).Vertices));
    CHECK(VecApprox(cube.Min, vec3(-0.5f)));
    CHECK(VecApprox(cube.Max, vec3(0.5f)));

    const f32 radius = 0.75f;
    const AABB sphere = Mesh::ComputeBounds(
        std::span<const CanonicalVertex>(Primitives::Sphere(radius, 16, 24).Vertices));
    CHECK(sphere.Min.x == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(sphere.Max.x == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(sphere.Min.y == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(sphere.Max.y == doctest::Approx(+radius).epsilon(0.001f));
    CHECK(sphere.Min.z == doctest::Approx(-radius).epsilon(0.001f));
    CHECK(sphere.Max.z == doctest::Approx(+radius).epsilon(0.001f));

    // Plane is flat on Y; its bound is degenerate (zero extent) on that axis.
    const vec2 size(3.0f, 5.0f);
    const AABB plane =
        Mesh::ComputeBounds(std::span<const CanonicalVertex>(Primitives::Plane(size).Vertices));
    CHECK(plane.Min.y == doctest::Approx(0.0f));
    CHECK(plane.Max.y == doctest::Approx(0.0f));
    CHECK(plane.Min.x == doctest::Approx(-size.x * 0.5f));
    CHECK(plane.Max.x == doctest::Approx(+size.x * 0.5f));
    CHECK(plane.Min.z == doctest::Approx(-size.y * 0.5f));
    CHECK(plane.Max.z == doctest::Approx(+size.y * 0.5f));
}

namespace
{
    // A device-free Mesh: only a name + bound, no GPU buffers. SceneBounds reads
    // GetBounds() and never touches the (empty) vertex/index buffers.
    Ref<Mesh> BoundsMesh(const AABB& bounds)
    {
        return Mesh::Create(MeshInfo{.Name = "test", .Bounds = bounds});
    }
}

TEST_CASE("SceneBounds: union of two primitives at known transforms")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    types.Register<Name>("Name");
    types.Register<Transform>("Transform");
    types.Register<Parent>("Parent");
    types.Register<MeshRenderer>("MeshRenderer");

    AssetManager manager(context, tasks, types);

    Unique<Scene> scene = Scene::Create(types);

    // A unit cube mesh placed at +X 10 and a unit cube at -X 10; the scene bound
    // spans both.
    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    const Entity a = scene->CreateEntity();
    scene->Add<Transform>(a, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(a, MeshRenderer{.Mesh = mesh});

    const Entity b = scene->CreateEntity();
    scene->Add<Transform>(b, Transform{.Position = vec3(-10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(b, MeshRenderer{.Mesh = mesh});

    const AABB bounds = SceneBounds(*scene);
    CHECK_FALSE(bounds.IsEmpty());
    CHECK(bounds.Min.x == doctest::Approx(-10.5f));
    CHECK(bounds.Max.x == doctest::Approx(10.5f));
    CHECK(bounds.Min.y == doctest::Approx(-0.5f));
    CHECK(bounds.Max.y == doctest::Approx(0.5f));
    CHECK(bounds.Min.z == doctest::Approx(-0.5f));
    CHECK(bounds.Max.z == doctest::Approx(0.5f));
}

TEST_CASE("SceneBounds: a non-resident mesh contributes nothing")
{
    const Renderer::Context context;
    const TaskSystem tasks;
    TypeRegistry types;
    types.Register<Name>("Name");
    types.Register<Transform>("Transform");
    types.Register<Parent>("Parent");
    types.Register<MeshRenderer>("MeshRenderer");

    Unique<Scene> scene = Scene::Create(types);

    // A MeshRenderer with a default (unloaded) handle is skipped.
    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e, Transform{.Position = vec3(5.0f)});
    scene->Add<MeshRenderer>(e, MeshRenderer{});

    CHECK(SceneBounds(*scene).IsEmpty());
}

TEST_CASE("SceneBounds: an empty scene returns AABB::Empty")
{
    TypeRegistry types;
    types.Register<Name>("Name");
    types.Register<Transform>("Transform");
    types.Register<Parent>("Parent");
    types.Register<MeshRenderer>("MeshRenderer");

    const Unique<Scene> scene = Scene::Create(types);
    CHECK(SceneBounds(*scene).IsEmpty());
}
