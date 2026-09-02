// The per-frame visibility gather: a device-free scene reduction. Meshes are
// built through the MeshInfo factory (a name + bound, empty buffers), enough for
// GatherMeshes to read GetBounds() and AssetManager::Adopt to wrap them in
// resident handles. No Context, no Vulkan — the gather is pure scene-query math.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Math/AABB.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RenderLayer.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/Visibility.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    void RegisterBuiltins(TypeRegistry& types)
    {
        types.Register<Name>("Name");
        types.Register<Transform>("Transform");
        types.Register<Hierarchy>("Hierarchy");
        types.Register<MeshRenderer>("MeshRenderer");
    }

    // A device-free Mesh: only a name + bound, no GPU buffers. The gather reads
    // GetBounds() and never touches the (empty) vertex/index buffers.
    Ref<Mesh> BoundsMesh(const AABB& bounds)
    {
        return Mesh::Create(MeshInfo{.Name = "test", .Bounds = bounds});
    }
}

TEST_CASE("GatherMeshes: two mesh entities yield two VisibleMeshes in dense order")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AABB unit{.Min = vec3(-0.5f), .Max = vec3(0.5f)};
    const AssetHandle<Mesh> mesh = manager.Adopt<Mesh>(BoundsMesh(unit));

    const Entity a = scene->CreateEntity();
    scene->Add<Transform>(a, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(a, MeshRenderer{.Mesh = mesh});

    const Entity b = scene->CreateEntity();
    scene->Add<Transform>(b, Transform{.Position = vec3(-10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(b, MeshRenderer{.Mesh = mesh});

    vector<VisibleMesh> out;
    AABB outBounds = AABB::Empty();
    GatherMeshes(*scene, out, outBounds);

    REQUIRE(out.size() == 2);

    // Transform-pool dense order: a then b.
    CHECK(out[0].Owner == a);
    CHECK(out[1].Owner == b);

    CHECK(out[0].Mesh == mesh.Get());
    CHECK(out[1].Mesh == mesh.Get());

    // The world matrix is each entity's WorldMatrix; the world bound is the local
    // bound transformed by it.
    const mat4 worldA = WorldMatrix(*scene, a);
    const mat4 worldB = WorldMatrix(*scene, b);
    CHECK(out[0].World == worldA);
    CHECK(out[1].World == worldB);

    const AABB expectedA = unit.Transformed(worldA);
    const AABB expectedB = unit.Transformed(worldB);
    CHECK(out[0].WorldBounds.Min.x == doctest::Approx(expectedA.Min.x));
    CHECK(out[0].WorldBounds.Max.x == doctest::Approx(expectedA.Max.x));
    CHECK(out[1].WorldBounds.Min.x == doctest::Approx(expectedB.Min.x));
    CHECK(out[1].WorldBounds.Max.x == doctest::Approx(expectedB.Max.x));
}

TEST_CASE("GatherMeshes: non-resident and MeshRenderer-less entities contribute nothing")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    // A MeshRenderer with a default (unloaded) handle.
    const Entity nonResident = scene->CreateEntity();
    scene->Add<Transform>(nonResident, Transform{.Position = vec3(5.0f)});
    scene->Add<MeshRenderer>(nonResident, MeshRenderer{});

    // A Transform with no MeshRenderer.
    const Entity transformOnly = scene->CreateEntity();
    scene->Add<Transform>(transformOnly, Transform{.Position = vec3(-5.0f)});

    // One genuinely resident entity to prove the others are simply skipped.
    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-1.0f), .Max = vec3(1.0f)}));
    const Entity resident = scene->CreateEntity();
    scene->Add<Transform>(resident, Transform{});
    scene->Add<MeshRenderer>(resident, MeshRenderer{.Mesh = mesh});

    vector<VisibleMesh> out;
    AABB outBounds = AABB::Empty();
    GatherMeshes(*scene, out, outBounds);

    REQUIRE(out.size() == 1);
    CHECK(out[0].Owner == resident);
}

TEST_CASE("GatherMeshes: an empty scene yields an empty list and empty bounds")
{
    TypeRegistry types;
    RegisterBuiltins(types);
    const Unique<Scene> scene = Scene::Create(types);

    vector<VisibleMesh> out;
    AABB outBounds{.Min = vec3(-99.0f), .Max = vec3(99.0f)};
    GatherMeshes(*scene, out, outBounds);

    CHECK(out.empty());
    CHECK(outBounds.IsEmpty());
}

TEST_CASE("GatherMeshes: outBounds equals SceneBounds (the by-product agrees)")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    const Entity a = scene->CreateEntity();
    scene->Add<Transform>(a, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(a, MeshRenderer{.Mesh = mesh});

    const Entity b = scene->CreateEntity();
    scene->Add<Transform>(b, Transform{.Position = vec3(-10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(b, MeshRenderer{.Mesh = mesh});

    vector<VisibleMesh> out;
    AABB outBounds = AABB::Empty();
    GatherMeshes(*scene, out, outBounds);

    const AABB standalone = SceneBounds(*scene);
    CHECK(outBounds.Min.x == doctest::Approx(standalone.Min.x));
    CHECK(outBounds.Min.y == doctest::Approx(standalone.Min.y));
    CHECK(outBounds.Min.z == doctest::Approx(standalone.Min.z));
    CHECK(outBounds.Max.x == doctest::Approx(standalone.Max.x));
    CHECK(outBounds.Max.y == doctest::Approx(standalone.Max.y));
    CHECK(outBounds.Max.z == doctest::Approx(standalone.Max.z));
}

TEST_CASE("GatherMeshes: a pre-filled out vector is cleared first")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));
    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e, Transform{});
    scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});

    // Seed stale entries; the gather must drop them.
    vector<VisibleMesh> out(3);
    AABB outBounds = AABB::Empty();
    GatherMeshes(*scene, out, outBounds);

    CHECK(out.size() == 1);
    CHECK(out[0].Owner == e);
}

TEST_CASE("MeshRenderer::Visible excludes a mesh from the gather and from the scene bound")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    const Entity shown = scene->CreateEntity();
    scene->Add<Transform>(shown, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(shown, MeshRenderer{.Mesh = mesh});

    const Entity hidden = scene->CreateEntity();
    scene->Add<Transform>(hidden, Transform{.Position = vec3(-100.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(hidden, MeshRenderer{.Mesh = mesh, .Visible = false});

    vector<VisibleMesh> out;
    AABB outBounds = AABB::Empty();
    GatherMeshes(*scene, out, outBounds);

    // Only the visible one is a draw, and the hidden one does not stretch the bound the shadow
    // projections fit to — which is the half a CastsShadows-only flag would have left behind.
    CHECK(out.size() == 1);
    CHECK(out[0].Owner == shown);
    CHECK(outBounds.Min.x == doctest::Approx(9.5f));
    CHECK(SceneBounds(*scene).Min.x == doctest::Approx(9.5f));

    // The switch is live and symmetric: flipping it back restores the draw exactly.
    scene->Get<MeshRenderer>(hidden).Visible = true;
    GatherMeshes(*scene, out, outBounds);
    CHECK(out.size() == 2);
    CHECK(outBounds.Min.x == doctest::Approx(-100.5f));

    // Hiding a mesh is not the same as dropping its renderer: the handle survives, so nothing
    // has to be re-resolved to show it again.
    CHECK(scene->Get<MeshRenderer>(hidden).Mesh.IsLoaded());
}

TEST_CASE("GatherMeshes: a layer mask excludes off-mask renderers")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    const AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AABB unit{.Min = vec3(-0.5f), .Max = vec3(0.5f)};
    const AssetHandle<Mesh> mesh = manager.Adopt<Mesh>(BoundsMesh(unit));

    const Entity ordinary = scene->CreateEntity();
    scene->Add<Transform>(ordinary, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(ordinary, MeshRenderer{.Mesh = mesh, .Layer = RenderLayer::Default});

    const Entity anchored = scene->CreateEntity();
    scene->Add<Transform>(anchored, Transform{.Position = vec3(-100.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(anchored,
                             MeshRenderer{.Mesh = mesh, .Layer = RenderLayer::ViewAnchored});

    vector<VisibleMesh> out;
    AABB outBounds = AABB::Empty();

    // The default mask draws every layer, so both entities are candidates and the anchored one
    // still widens the bound.
    GatherMeshes(*scene, out, outBounds);
    CHECK(out.size() == 2);
    CHECK(outBounds.Min.x == doctest::Approx(-100.5f));

    // The environment-capture mask drops the view-anchored one — absent from the candidate list and
    // no longer stretching the bound — which is the whole of the dust-in-the-lens fix.
    GatherMeshes(*scene, out, outBounds, Entity::Null, DefaultEnvironmentCaptureLayers);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Owner == ordinary);
    CHECK(outBounds.Min.x == doctest::Approx(9.5f));

    // The exclude and the mask compose: excluding the only on-mask entity leaves nothing.
    GatherMeshes(*scene, out, outBounds, ordinary, DefaultEnvironmentCaptureLayers);
    CHECK(out.empty());
}
