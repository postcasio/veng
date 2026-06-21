// ECS query and transform-hierarchy unit cases: multi-component Each/View over
// sparse-set pools, and the LocalMatrix/WorldMatrix/ComputeWorldMatrices walk.
// Pure CPU — no Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <vector>

#include <glm/gtc/epsilon.hpp>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

using namespace Veng;

namespace
{
    struct A
    {
        int Value = 0;
    };
    struct B
    {
        int Value = 0;
    };
    struct C
    {
        int Value = 0;
    };
}

VE_TYPE(A, 0x6D2A1F8C44B07E31ULL);
VE_TYPE(B, 0x9F31C0A5E2741B6DULL);
VE_TYPE(C, 0x18AE73D6905C2F44ULL);

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<A>("A");
        registry.Register<B>("B");
        registry.Register<C>("C");
        registry.Register<Name>("Name");
        registry.Register<Transform>("Transform");
        registry.Register<Hierarchy>("Hierarchy");
        return registry;
    }

    bool MatrixApproxEqual(const mat4& lhs, const mat4& rhs, f32 eps = 1e-4f)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (std::abs(lhs[c][r] - rhs[c][r]) > eps)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool VecApproxEqual(const vec3& lhs, const vec3& rhs, f32 eps = 1e-4f)
    {
        return glm::all(glm::epsilonEqual(lhs, rhs, eps));
    }
}

// --- Single- and multi-component queries ------------------------------------

TEST_CASE("Each<A> visits exactly the entities holding A, in dense order")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e0 = scene->CreateEntity();
    const Entity e1 = scene->CreateEntity();
    const Entity e2 = scene->CreateEntity();

    scene->Add<A>(e0, A{10});
    scene->Add<A>(e2, A{12});
    // e1 has no A.

    std::vector<Entity> visited;
    std::vector<int> values;
    scene->Each<A>(
        [&](Entity e, A& a)
        {
            visited.push_back(e);
            values.push_back(a.Value);
        });

    CHECK(visited.size() == 2);
    CHECK(visited[0] == e0);
    CHECK(visited[1] == e2);
    CHECK(values[0] == 10);
    CHECK(values[1] == 12);
}

TEST_CASE("Each<A,B> visits only entities holding both")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity ab = scene->CreateEntity();
    const Entity aOnly = scene->CreateEntity();
    const Entity bOnly = scene->CreateEntity();
    const Entity both2 = scene->CreateEntity();

    scene->Add<A>(ab, A{1});
    scene->Add<B>(ab, B{2});
    scene->Add<A>(aOnly, A{3});
    scene->Add<B>(bOnly, B{4});
    scene->Add<A>(both2, A{5});
    scene->Add<B>(both2, B{6});

    std::vector<Entity> visited;
    scene->Each<A, B>(
        [&](Entity e, A& a, B& b)
        {
            visited.push_back(e);
            CHECK(a.Value + 1 == b.Value);
        });

    CHECK(visited.size() == 2);
    CHECK(visited[0] == ab);
    CHECK(visited[1] == both2);
}

TEST_CASE("Each<A,B,C> visits only entities holding all three")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity all = scene->CreateEntity();
    const Entity ab = scene->CreateEntity();
    const Entity ac = scene->CreateEntity();

    scene->Add<A>(all);
    scene->Add<B>(all);
    scene->Add<C>(all);
    scene->Add<A>(ab);
    scene->Add<B>(ab);
    scene->Add<A>(ac);
    scene->Add<C>(ac);

    std::vector<Entity> visited;
    scene->Each<A, B, C>([&](Entity e, A&, B&, C&) { visited.push_back(e); });

    CHECK(visited.size() == 1);
    CHECK(visited[0] == all);
}

TEST_CASE("An empty intersection visits nothing")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();
    scene->Add<A>(e);
    // No entity has B, so Each<A,B> is empty. Querying a never-pooled type (C)
    // is also empty and must not crash.

    int count = 0;
    scene->Each<A, B>([&](Entity, A&, B&) { ++count; });
    CHECK(count == 0);

    scene->Each<C>([&](Entity, C&) { ++count; });
    CHECK(count == 0);
}

TEST_CASE("Mutating through the query reference persists")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();
    scene->Add<A>(e, A{1});

    scene->Each<A>([](Entity, A& a) { a.Value = 99; });
    CHECK(scene->Get<A>(e).Value == 99);
}

// --- Smallest-pool driver ---------------------------------------------------

TEST_CASE("Smallest pool drives the query; intersection is correct")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // Many A's, few B's. The B pool (smaller) must drive, but the result is the
    // same intersection regardless of which side is added first.
    std::vector<Entity> aEntities;
    for (int i = 0; i < 50; ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<A>(e, A{i});
        aEntities.push_back(e);
    }
    // Give B to just two of them.
    scene->Add<B>(aEntities[10], B{0});
    scene->Add<B>(aEntities[40], B{0});

    std::vector<Entity> visited;
    scene->Each<A, B>([&](Entity e, A&, B&) { visited.push_back(e); });

    CHECK(visited.size() == 2);
    // Driver is the B pool, so order is B's dense order (insertion order).
    CHECK(visited[0] == aEntities[10]);
    CHECK(visited[1] == aEntities[40]);
}

// --- View range-for + early-out ---------------------------------------------

TEST_CASE("View range-for visits the intersection")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e0 = scene->CreateEntity();
    const Entity e1 = scene->CreateEntity();
    scene->Add<A>(e0, A{1});
    scene->Add<B>(e0, B{2});
    scene->Add<A>(e1, A{3});
    scene->Add<B>(e1, B{4});

    int sum = 0;
    for (auto [e, a, b] : scene->View<A, B>())
    {
        (void)e;
        sum += a.Value + b.Value;
    }
    CHECK(sum == 10);
}

TEST_CASE("View early-out with break stops without visiting the rest")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    std::vector<Entity> entities;
    for (int i = 0; i < 5; ++i)
    {
        const Entity e = scene->CreateEntity();
        scene->Add<A>(e, A{i});
        entities.push_back(e);
    }

    int visited = 0;
    for (auto [e, a] : scene->View<A>())
    {
        (void)e;
        (void)a;
        ++visited;
        if (visited == 2)
        {
            break;
        }
    }
    CHECK(visited == 2);
}

TEST_CASE("View skips driver entries missing another component")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity a0 = scene->CreateEntity();
    const Entity a1 = scene->CreateEntity();
    const Entity a2 = scene->CreateEntity();
    scene->Add<A>(a0);
    scene->Add<A>(a1);
    scene->Add<A>(a2);
    // B only on a1.
    scene->Add<B>(a1);

    std::vector<Entity> visited;
    for (auto [e, a, b] : scene->View<A, B>())
    {
        (void)a;
        (void)b;
        visited.push_back(e);
    }
    CHECK(visited.size() == 1);
    CHECK(visited[0] == a1);
}

// --- LocalMatrix ------------------------------------------------------------

TEST_CASE("LocalMatrix applies T*R*S to a point")
{
    Transform t;
    t.Position = vec3(10.0f, 0.0f, 0.0f);
    t.Scale = vec3(2.0f, 2.0f, 2.0f);
    // 90° about +Z maps +X → +Y.
    t.Rotation = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 0.0f, 1.0f));

    const mat4 m = LocalMatrix(t);
    const vec3 p = vec3(m * vec4(1.0f, 0.0f, 0.0f, 1.0f));
    // (1,0,0) scaled by 2 → (2,0,0), rotated 90°Z → (0,2,0), translated +10x →
    // (10,2,0).
    CHECK(VecApproxEqual(p, vec3(10.0f, 2.0f, 0.0f)));
}

// --- WorldMatrix / hierarchy ------------------------------------------------

TEST_CASE("WorldMatrix of a root equals its LocalMatrix")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity root = scene->CreateEntity();
    auto& t = scene->Add<Transform>(root);
    t.Position = vec3(3.0f, 4.0f, 5.0f);

    CHECK(MatrixApproxEqual(WorldMatrix(*scene, root), LocalMatrix(t)));
}

TEST_CASE("WorldMatrix composes a 3-deep parent chain")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity root = scene->CreateEntity();
    const Entity mid = scene->CreateEntity();
    const Entity leaf = scene->CreateEntity();

    scene->Add<Transform>(root).Position = vec3(10.0f, 0.0f, 0.0f);
    scene->Add<Transform>(mid).Position = vec3(0.0f, 5.0f, 0.0f);
    scene->Add<Transform>(leaf).Position = vec3(0.0f, 0.0f, 2.0f);
    scene->SetParent(mid, root);
    scene->SetParent(leaf, mid);

    const mat4 world = WorldMatrix(*scene, leaf);
    const vec3 origin = vec3(world * vec4(0.0f, 0.0f, 0.0f, 1.0f));
    // Pure translations compose additively: (10,5,2).
    CHECK(VecApproxEqual(origin, vec3(10.0f, 5.0f, 2.0f)));

    const mat4 expected = LocalMatrix(scene->Get<Transform>(root)) *
                          LocalMatrix(scene->Get<Transform>(mid)) *
                          LocalMatrix(scene->Get<Transform>(leaf));
    CHECK(MatrixApproxEqual(world, expected));
}

TEST_CASE("Reparenting changes the world matrix")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity child = scene->CreateEntity();

    scene->Add<Transform>(a).Position = vec3(100.0f, 0.0f, 0.0f);
    scene->Add<Transform>(b).Position = vec3(0.0f, 200.0f, 0.0f);
    scene->Add<Transform>(child).Position = vec3(1.0f, 1.0f, 1.0f);

    scene->SetParent(child, a);
    const vec3 underA = vec3(WorldMatrix(*scene, child) * vec4(0, 0, 0, 1));
    CHECK(VecApproxEqual(underA, vec3(101.0f, 1.0f, 1.0f)));

    scene->SetParent(child, b);
    const vec3 underB = vec3(WorldMatrix(*scene, child) * vec4(0, 0, 0, 1));
    CHECK(VecApproxEqual(underB, vec3(1.0f, 201.0f, 1.0f)));
}

TEST_CASE("ComputeWorldMatrices matches per-entity WorldMatrix for a forest")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // Tree 1: r1 → c1. Tree 2: r2 (lone root). Plus a non-Transform entity.
    const Entity r1 = scene->CreateEntity();
    const Entity c1 = scene->CreateEntity();
    const Entity r2 = scene->CreateEntity();
    const Entity noTransform = scene->CreateEntity();

    scene->Add<Transform>(r1).Position = vec3(1.0f, 0.0f, 0.0f);
    scene->Add<Transform>(c1).Position = vec3(0.0f, 1.0f, 0.0f);
    scene->Add<Transform>(r2).Position = vec3(0.0f, 0.0f, 1.0f);
    scene->SetParent(c1, r1);
    scene->Add<A>(noTransform);

    std::vector<mat4> all;
    ComputeWorldMatrices(*scene, all);

    // One matrix per Transform-bearing entity (3), none for noTransform.
    CHECK(all.size() == 3);

    // Walk the Transform pool's dense order via the same query the function uses
    // and confirm element-wise agreement with per-entity WorldMatrix.
    std::vector<Entity> transformEntities;
    scene->Each<Transform>([&](Entity e, Transform&) { transformEntities.push_back(e); });
    REQUIRE(transformEntities.size() == all.size());
    for (usize i = 0; i < all.size(); ++i)
    {
        CHECK(MatrixApproxEqual(all[i], WorldMatrix(*scene, transformEntities[i])));
    }
}
