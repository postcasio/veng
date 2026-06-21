// The Scene's intrusive Hierarchy topology operations: SetParent / Detach /
// MoveBefore / GetParent / ForEachChild and recursive DestroyEntity over the
// sibling-linked structure. Pure CPU — no Context, no Vulkan symbol touched.
//
// Each case asserts the invariant the up-link could not: a child's Parent always
// links back, and a parent's child list contains exactly its children in order.
// The cycle assert is API misuse (std::abort) and is covered by the death suite.

#include <doctest/doctest.h>

#include <vector>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Name>("Name");
        registry.Register<Transform>("Transform");
        registry.Register<Hierarchy>("Hierarchy");
        return registry;
    }

    // The ordered direct children of `entity`, gathered via ForEachChild.
    std::vector<Entity> ChildrenOf(const Scene& scene, Entity entity)
    {
        std::vector<Entity> out;
        scene.ForEachChild(entity, [&](Entity child) { out.push_back(child); });
        return out;
    }
}

TEST_CASE("SetParent attaches a child and links it back")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity child = scene->CreateEntity();

    scene->SetParent(child, parent);

    CHECK(scene->GetParent(child) == parent);

    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 1);
    CHECK(children[0] == child);
}

TEST_CASE("SetParent appends children in insertion order")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == a);
    CHECK(children[1] == b);
    CHECK(children[2] == c);

    for (const Entity child : children)
    {
        CHECK(scene->GetParent(child) == parent);
    }
}

TEST_CASE("Detach removes a child from its parent's list and clears the up-link")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);

    scene->Detach(a);

    CHECK(scene->GetParent(a).IsNull());

    // b remains, now the sole child, with its links intact.
    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 1);
    CHECK(children[0] == b);
    CHECK(scene->GetParent(b) == parent);
}

TEST_CASE("Detaching the middle child keeps the rest of the list contiguous")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    scene->Detach(b);

    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 2);
    CHECK(children[0] == a);
    CHECK(children[1] == c);
    CHECK(scene->GetParent(b).IsNull());
}

TEST_CASE("SetParent reparents a child between two parents")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity p1 = scene->CreateEntity();
    const Entity p2 = scene->CreateEntity();
    const Entity child = scene->CreateEntity();

    scene->SetParent(child, p1);
    CHECK(scene->GetParent(child) == p1);
    REQUIRE(ChildrenOf(*scene, p1).size() == 1);

    scene->SetParent(child, p2);
    CHECK(scene->GetParent(child) == p2);
    // The old parent's list is now empty; the new parent owns the child.
    CHECK(ChildrenOf(*scene, p1).empty());
    const std::vector<Entity> p2Children = ChildrenOf(*scene, p2);
    REQUIRE(p2Children.size() == 1);
    CHECK(p2Children[0] == child);
}

TEST_CASE("MoveBefore reorders siblings under the same parent")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    // Move c before a: order becomes c, a, b.
    scene->MoveBefore(c, a);

    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == c);
    CHECK(children[1] == a);
    CHECK(children[2] == b);
}

TEST_CASE("MoveBefore inserts before a middle sibling")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    // Move a before c: order becomes b, a, c.
    scene->MoveBefore(a, c);

    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == b);
    CHECK(children[1] == a);
    CHECK(children[2] == c);
}

TEST_CASE("MoveBefore reparents the child under the sibling's parent")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity p1 = scene->CreateEntity();
    const Entity p2 = scene->CreateEntity();
    const Entity child = scene->CreateEntity();
    const Entity target = scene->CreateEntity();

    scene->SetParent(child, p1);
    scene->SetParent(target, p2);

    // child lives under p1, target under p2; moving child before target pulls it
    // into p2's list.
    scene->MoveBefore(child, target);

    CHECK(scene->GetParent(child) == p2);
    CHECK(ChildrenOf(*scene, p1).empty());
    const std::vector<Entity> p2Children = ChildrenOf(*scene, p2);
    REQUIRE(p2Children.size() == 2);
    CHECK(p2Children[0] == child);
    CHECK(p2Children[1] == target);
}

TEST_CASE("GetParent and ForEachChild are empty for an entity with no Hierarchy")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity lone = scene->CreateEntity();

    CHECK(scene->GetParent(lone).IsNull());
    CHECK(ChildrenOf(*scene, lone).empty());
}

TEST_CASE("DestroyEntity removes the whole subtree and keeps siblings consistent")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // root → { a → grandchild, b }, plus a surviving sibling root.
    const Entity root = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity grandchild = scene->CreateEntity();

    scene->SetParent(a, root);
    scene->SetParent(b, root);
    scene->SetParent(grandchild, a);

    const Entity otherRoot = scene->CreateEntity();
    const Entity otherChild = scene->CreateEntity();
    scene->SetParent(otherChild, otherRoot);

    scene->DestroyEntity(root);

    CHECK_FALSE(scene->IsAlive(root));
    CHECK_FALSE(scene->IsAlive(a));
    CHECK_FALSE(scene->IsAlive(b));
    CHECK_FALSE(scene->IsAlive(grandchild));

    // The unrelated subtree is untouched and still linked.
    CHECK(scene->IsAlive(otherRoot));
    CHECK(scene->IsAlive(otherChild));
    CHECK(scene->GetParent(otherChild) == otherRoot);
    REQUIRE(ChildrenOf(*scene, otherRoot).size() == 1);
    CHECK(ChildrenOf(*scene, otherRoot)[0] == otherChild);
}

TEST_CASE("Destroying a child detaches it from a surviving parent's list")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();

    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    scene->DestroyEntity(b);

    CHECK(scene->IsAlive(parent));
    CHECK_FALSE(scene->IsAlive(b));

    // The parent's list closes over the gap: a, c remain, in order.
    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 2);
    CHECK(children[0] == a);
    CHECK(children[1] == c);
}

TEST_CASE("Detach to root then re-attach round-trips the links")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity parent = scene->CreateEntity();
    const Entity child = scene->CreateEntity();

    scene->SetParent(child, parent);
    scene->Detach(child);
    CHECK(scene->GetParent(child).IsNull());
    CHECK(ChildrenOf(*scene, parent).empty());

    scene->SetParent(child, parent);
    CHECK(scene->GetParent(child) == parent);
    const std::vector<Entity> children = ChildrenOf(*scene, parent);
    REQUIRE(children.size() == 1);
    CHECK(children[0] == child);
}
