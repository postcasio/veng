// The SceneBroadphase integration: the version-gate → rebuild → cull path tying
// the Scene spatial-version counter and the BVH together. Device-free — meshes are
// built through the MeshInfo factory (a name + bound, empty buffers) and adopted
// into resident handles, the gather/build/query are pure CPU. This is the guard the
// byte-identical golden cannot provide: that the tree culls exactly the linear scan,
// rebuilds when (and only when) the scene moves, and tracks residency.

#include <doctest/doctest.h>

#include <algorithm>
#include <random>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneBroadphase.h>
#include <Veng/Scene/Visibility.h>
#include <Veng/Task/TaskSystem.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Veng;

namespace
{
    void RegisterBuiltins(TypeRegistry& types)
    {
        types.Register<Name>("Name");
        types.Register<Transform>("Transform");
        types.Register<Hierarchy>("Hierarchy");
        types.Register<MeshRenderer>("MeshRenderer");
        types.Register<Light>("Light");
    }

    // A device-free Mesh: a name, a whole-range submesh carrying the local bound, and
    // the same value as the mesh bound — no GPU buffers. The broadphase emits one leaf
    // per submesh, so a single-submesh mesh maps a candidate id 1:1 to its mesh index;
    // the gather reads GetBounds() and never touches the (empty) vertex/index buffers.
    Ref<Mesh> BoundsMesh(const AABB& bounds)
    {
        return Mesh::Create(MeshInfo{
            .Name = "test",
            .SubMeshes = {SubMesh{.IndexOffset = 0, .IndexCount = 0, .Bounds = bounds}},
            .Bounds = bounds,
        });
    }

    // A device-free Mesh with two submeshes carrying the given local bounds. The
    // broadphase emits one leaf per submesh; the whole-mesh bound is their union.
    Ref<Mesh> TwoSubMeshMesh(const AABB& left, const AABB& right)
    {
        AABB whole = left;
        whole.Expand(right);
        return Mesh::Create(MeshInfo{
            .Name = "two submeshes",
            .SubMeshes =
                {
                    SubMesh{.IndexOffset = 0, .IndexCount = 0, .Bounds = left},
                    SubMesh{.IndexOffset = 0, .IndexCount = 0, .Bounds = right},
                },
            .Bounds = whole,
        });
    }

    // The exact set the broadphase's Cull must reproduce: every candidate whose
    // tight world bound passes the same Intersects, in ascending (gather) order.
    vector<u32> LinearScan(std::span<const VisibleMesh> candidates, const Frustum& frustum)
    {
        vector<u32> ids;
        for (u32 i = 0; i < candidates.size(); ++i)
        {
            if (Intersects(frustum, candidates[i].WorldBounds))
            {
                ids.push_back(i);
            }
        }
        return ids; // already ascending — i is monotonic
    }

    Camera MakeCameraAt(const vec3& eye, const vec3& target)
    {
        Camera camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 500.0f);
        camera.SetView(eye, target, vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // A bank of varied frustums (randomized cameras + an ortho slab) the cull is
    // checked against.
    vector<Frustum> MakeFrustums(std::mt19937& rng)
    {
        std::uniform_real_distribution<f32> pos(-50.0f, 50.0f);
        vector<Frustum> frustums;
        for (i32 i = 0; i < 10; ++i)
        {
            const vec3 eye(pos(rng), pos(rng), pos(rng));
            const vec3 target(pos(rng), pos(rng), pos(rng));
            if (glm::distance(eye, target) < 1.0f)
            {
                continue;
            }
            frustums.push_back(
                Frustum::FromViewProjection(MakeCameraAt(eye, target).ViewProjection()));
        }
        frustums.push_back(
            Frustum::FromViewProjection(glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 80.0f)));
        return frustums;
    }
}

TEST_CASE("SceneBroadphase: Cull equals the linear tight scan, ascending, over many frustums")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<f32> pos(-40.0f, 40.0f);
    for (i32 i = 0; i < 80; ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e, Transform{.Position = vec3(pos(rng), pos(rng), pos(rng))});
        scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});
    }

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);
    CHECK(broadphase.DidRebuildLastSync());
    REQUIRE(broadphase.GetCandidates().size() == 80);

    for (const Frustum& frustum : MakeFrustums(rng))
    {
        vector<u32> culled;
        broadphase.Cull(frustum, culled);

        const vector<u32> expected = LinearScan(broadphase.GetCandidates(), frustum);
        CHECK(culled == expected); // identical set AND ascending order

        // Ascending in its own right (the contract the draw order depends on).
        CHECK(std::ranges::is_sorted(culled));
    }
}

TEST_CASE("SceneBroadphase: Cull appends to caller scratch, never clears it")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));
    const Entity e = scene->CreateEntity();
    // Place the box where the ortho frustum below (near 1, far 100, looking down -z)
    // contains it, so the cull keeps it and the append grows the vector.
    scene->Add<Transform>(e, Transform{.Position = vec3(0.0f, 0.0f, -10.0f)});
    scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);

    const Frustum wide =
        Frustum::FromViewProjection(glm::ortho(-100.0f, 100.0f, -100.0f, 100.0f, 1.0f, 100.0f));

    vector<u32> out = {999u}; // a pre-seeded entry the append must preserve
    broadphase.Cull(wide, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 999u);
}

TEST_CASE("SceneBroadphase: version gate — a second Sync of an unmutated scene does not rebuild")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));
    for (i32 i = 0; i < 4; ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e,
                              Transform{.Position = vec3(static_cast<f32>(i) * 3.0f, 0.0f, 0.0f)});
        scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});
    }

    SceneBroadphase broadphase;

    // The broadphase reads the scene const-only: every Sync leaves the spatial
    // version exactly where it found it.
    const u64 v0 = scene->GetSpatialVersion();
    broadphase.Sync(*scene);
    CHECK(broadphase.DidRebuildLastSync()); // first Sync always rebuilds
    CHECK(scene->GetSpatialVersion() == v0);

    const u64 v1 = scene->GetSpatialVersion();
    broadphase.Sync(*scene);
    CHECK_FALSE(broadphase.DidRebuildLastSync()); // unmutated → no rebuild
    CHECK(scene->GetSpatialVersion() == v1);
}

TEST_CASE("SceneBroadphase: every spatial mutation rebuilds and the tree stays correct")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    const AssetHandle<Mesh> mesh =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    auto AddMesh = [&](vec3 position) -> Entity
    {
        const Entity e = scene->CreateEntity();
        scene->Add<Transform>(e, Transform{.Position = position});
        scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});
        return e;
    };

    const Entity a = AddMesh(vec3(0.0f, 0.0f, 0.0f));
    const Entity b = AddMesh(vec3(5.0f, 0.0f, 0.0f));
    AddMesh(vec3(-5.0f, 0.0f, 0.0f));

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);
    REQUIRE(broadphase.DidRebuildLastSync());

    // After each mutation, the next Sync rebuilds AND the broadphase's cull equals a
    // linear scan and equals a fresh full-rebuild broadphase — so the rebuild
    // converges to the same tree regardless of the mutation history.
    auto CheckConverges = [&]()
    {
        CHECK(broadphase.DidRebuildLastSync());

        SceneBroadphase fresh;
        fresh.Sync(*scene);
        REQUIRE(fresh.GetCandidates().size() == broadphase.GetCandidates().size());

        std::mt19937 rng(0xBEEFu);
        for (const Frustum& frustum : MakeFrustums(rng))
        {
            vector<u32> mineCull, freshCull;
            broadphase.Cull(frustum, mineCull);
            fresh.Cull(frustum, freshCull);
            const vector<u32> expected = LinearScan(broadphase.GetCandidates(), frustum);
            CHECK(mineCull == expected);
            CHECK(mineCull == freshCull);
        }
    };

    SUBCASE("move (non-const Transform access)")
    {
        scene->Get<Transform>(a).Position = vec3(20.0f, 1.0f, -3.0f);
        broadphase.Sync(*scene);
        CheckConverges();
    }

    SUBCASE("add an entity")
    {
        AddMesh(vec3(12.0f, -4.0f, 8.0f));
        broadphase.Sync(*scene);
        CheckConverges();
    }

    SUBCASE("remove a component")
    {
        scene->Remove<MeshRenderer>(b);
        broadphase.Sync(*scene);
        CheckConverges();
        CHECK(broadphase.GetCandidates().size() == 2);
    }

    SUBCASE("reparent (SetParent link)")
    {
        scene->SetParent(b, a);
        broadphase.Sync(*scene);
        CheckConverges();
    }

    SUBCASE("destroy an entity")
    {
        scene->DestroyEntity(b);
        broadphase.Sync(*scene);
        CheckConverges();
        CHECK(broadphase.GetCandidates().size() == 2);
    }
}

TEST_CASE("SceneBroadphase: a mesh becoming resident between frames enters the tree (no spatial "
          "mutation)")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    // One genuinely resident mesh, plus one whose handle is held but not yet loaded:
    // adopt it (resident), then null its cache entry's Resource so IsLoaded() is
    // false. The handle shares the entry, so re-populating it later flips IsLoaded()
    // in place WITHOUT touching the scene — no spatial-version bump.
    const AssetHandle<Mesh> resident =
        manager.Adopt<Mesh>(BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)}));

    const Ref<Mesh> pendingMesh = BoundsMesh(AABB{.Min = vec3(-0.5f), .Max = vec3(0.5f)});
    const AssetHandle<Mesh> pending = manager.Adopt<Mesh>(pendingMesh);
    const Ref<Detail::AssetCacheEntry> entry = AssetManager::EntryOf(pending);
    REQUIRE(entry != nullptr);
    entry->Resource = nullptr; // make it not-yet-resident
    REQUIRE_FALSE(pending.IsLoaded());

    const Entity loaded = scene->CreateEntity();
    scene->Add<Transform>(loaded, Transform{.Position = vec3(0.0f)});
    scene->Add<MeshRenderer>(loaded, MeshRenderer{.Mesh = resident});

    const Entity stillLoading = scene->CreateEntity();
    scene->Add<Transform>(stillLoading, Transform{.Position = vec3(8.0f, 0.0f, 0.0f)});
    scene->Add<MeshRenderer>(stillLoading, MeshRenderer{.Mesh = pending});

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);
    REQUIRE(broadphase.DidRebuildLastSync());
    // Only the resident mesh is a candidate; the loading one is skipped by the gather.
    CHECK(broadphase.GetCandidates().size() == 1);

    // The mesh finishes loading — the cache entry's Resource is filled in place, with
    // no scene mutation. The version did not move, but the broadphase's pending poll
    // catches the residency change and rebuilds.
    const u64 versionBeforeLoad = scene->GetSpatialVersion();
    entry->Resource = std::static_pointer_cast<void>(pendingMesh);
    REQUIRE(pending.IsLoaded());
    CHECK(scene->GetSpatialVersion() == versionBeforeLoad); // residency is not a spatial mutation

    broadphase.Sync(*scene);
    CHECK(broadphase.DidRebuildLastSync());
    CHECK(broadphase.GetCandidates().size() == 2); // both meshes now in the tree

    // And a third Sync, nothing changed, does not rebuild — the pending set is now
    // empty, so there is nothing left to poll.
    broadphase.Sync(*scene);
    CHECK_FALSE(broadphase.DidRebuildLastSync());
}

TEST_CASE("SceneBroadphase: per-submesh leaves — a frustum drops one submesh of an on-screen mesh")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    RegisterBuiltins(types);

    AssetManager manager(context, tasks, types);
    Unique<Scene> scene = Scene::Create(types);

    // One wide mesh, two submeshes well separated on X: the left around x=-20, the
    // right around x=+20. Placed at the origin so its local bounds are its world bounds.
    const AABB left{.Min = vec3(-21.0f, -1.0f, -1.0f), .Max = vec3(-19.0f, 1.0f, 1.0f)};
    const AABB right{.Min = vec3(19.0f, -1.0f, -1.0f), .Max = vec3(21.0f, 1.0f, 1.0f)};
    const AssetHandle<Mesh> mesh = manager.Adopt<Mesh>(TwoSubMeshMesh(left, right));

    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e, Transform{.Position = vec3(0.0f)});
    scene->Add<MeshRenderer>(e, MeshRenderer{.Mesh = mesh});

    SceneBroadphase broadphase;
    broadphase.Sync(*scene);
    REQUIRE(broadphase.DidRebuildLastSync());

    // One mesh candidate, two per-submesh candidates.
    REQUIRE(broadphase.GetCandidates().size() == 1);
    REQUIRE(broadphase.GetSubMeshCandidates().size() == 2);
    CHECK(broadphase.GetSubMeshCandidates()[0].MeshCandidate == 0);
    CHECK(broadphase.GetSubMeshCandidates()[0].SubMeshIndex == 0);
    CHECK(broadphase.GetSubMeshCandidates()[1].SubMeshIndex == 1);

    // The union of the per-submesh world bounds equals the whole-mesh world bound.
    AABB unioned = AABB::Empty();
    unioned.Expand(left);
    unioned.Expand(right);
    const AABB whole = broadphase.GetCandidates()[0].WorldBounds;
    CHECK(unioned.Min.x == doctest::Approx(whole.Min.x));
    CHECK(unioned.Max.x == doctest::Approx(whole.Max.x));

    // A wide frustum containing the whole mesh returns BOTH submesh candidates.
    const Frustum wide =
        Frustum::FromViewProjection(glm::ortho(-50.0f, 50.0f, -50.0f, 50.0f, -50.0f, 50.0f));
    {
        vector<u32> culled;
        broadphase.Cull(wide, culled);
        CHECK(culled == vector<u32>{0u, 1u});
    }

    // A frustum covering only the right half misses the left submesh's bound entirely,
    // so the cull drops exactly that submesh's candidate — the per-submesh refinement.
    const Frustum rightOnly =
        Frustum::FromViewProjection(glm::ortho(10.0f, 50.0f, -50.0f, 50.0f, -50.0f, 50.0f));
    {
        vector<u32> culled;
        broadphase.Cull(rightOnly, culled);
        CHECK(culled == vector<u32>{1u}); // only the right submesh (candidate id 1)
    }

    // Symmetric: a left-only frustum keeps only the left submesh.
    const Frustum leftOnly =
        Frustum::FromViewProjection(glm::ortho(-50.0f, -10.0f, -50.0f, 50.0f, -50.0f, 50.0f));
    {
        vector<u32> culled;
        broadphase.Cull(leftOnly, culled);
        CHECK(culled == vector<u32>{0u});
    }
}
