// Scene::Clone: the deep-copy the editor's non-destructive Play mode relies on.
// Covers component-value copy, hierarchy-topology preservation, intra-scene Entity
// reference remapping into the clone, and full independence of the two scenes.
// Pure CPU — no Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    // A component holding an intra-scene Entity reference, exercising the
    // FieldClass::Reference remap path Clone performs (Entity is a Reference leaf).
    struct Target
    {
        Entity Other = Entity::Null;
        f32 Weight = 1.0f;
    };
}

VE_REFLECT(::Target, 0x6F2D9A4C1B8E5037ULL)
VE_FIELD(Other, .DisplayName = "Other")
VE_FIELD(Weight, .DisplayName = "Weight")
VE_REFLECT_END();

namespace
{
    // The field-populating zero-arg registration: Clone serializes component fields,
    // so the registry must carry each type's reflected FieldDescriptors (the one-arg
    // Register(name) overload registers lifecycle thunks only, with no fields).
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Hierarchy>();
        registry.Register<Target>();
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

TEST_CASE("Clone copies entity count and component field values")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity a = scene->CreateEntity();
    scene->Add<Name>(a, Name{.Value = "alpha"});
    scene->Add<Transform>(a).Position = vec3(1.0f, 2.0f, 3.0f);

    const Entity b = scene->CreateEntity();
    scene->Add<Name>(b, Name{.Value = "beta"});

    const Unique<Scene> copy = scene->Clone();

    CHECK(copy->EntityCount() == scene->EntityCount());

    // Field values survive the copy at the corresponding entity slots.
    const Entity ca{.Index = a.Index, .Generation = a.Generation};
    REQUIRE(copy->IsAlive(ca));
    REQUIRE(copy->Has<Name>(ca));
    CHECK(copy->Get<Name>(ca).Value == "alpha");
    REQUIRE(copy->Has<Transform>(ca));
    CHECK(copy->Get<Transform>(ca).Position.x == doctest::Approx(1.0f));
    CHECK(copy->Get<Transform>(ca).Position.z == doctest::Approx(3.0f));
}

TEST_CASE("Clone preserves the Hierarchy parent/child topology")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // root → { a, b }, a → grandchild.
    const Entity root = scene->CreateEntity();
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity grandchild = scene->CreateEntity();

    scene->SetParent(a, root);
    scene->SetParent(b, root);
    scene->SetParent(grandchild, a);

    const Unique<Scene> copy = scene->Clone();

    const Entity croot{.Index = root.Index, .Generation = root.Generation};
    const Entity ca{.Index = a.Index, .Generation = a.Generation};
    const Entity cb{.Index = b.Index, .Generation = b.Generation};
    const Entity cg{.Index = grandchild.Index, .Generation = grandchild.Generation};

    // The clone's links are rebuilt to match the source topology exactly.
    const std::vector<Entity> rootChildren = ChildrenOf(*copy, croot);
    REQUIRE(rootChildren.size() == 2);
    CHECK(rootChildren[0] == ca);
    CHECK(rootChildren[1] == cb);

    CHECK(copy->GetParent(ca) == croot);
    CHECK(copy->GetParent(cb) == croot);
    CHECK(copy->GetParent(cg) == ca);

    const std::vector<Entity> aChildren = ChildrenOf(*copy, ca);
    REQUIRE(aChildren.size() == 1);
    CHECK(aChildren[0] == cg);
}

TEST_CASE("Clone remaps an intra-scene Entity reference to the clone's entity")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity referrer = scene->CreateEntity();
    const Entity referent = scene->CreateEntity();
    scene->Add<Target>(referrer, Target{.Other = referent, .Weight = 2.0f});

    const Unique<Scene> copy = scene->Clone();

    const Entity cReferrer{.Index = referrer.Index, .Generation = referrer.Generation};
    const Entity cReferent{.Index = referent.Index, .Generation = referent.Generation};

    REQUIRE(copy->Has<Target>(cReferrer));
    const Target& cloned = copy->Get<Target>(cReferrer);

    // The reference points at the clone's referent, not the source scene's.
    CHECK(cloned.Other == cReferent);
    CHECK(cloned.Weight == doctest::Approx(2.0f));
}

TEST_CASE("Clone is independent: mutating one scene does not touch the other")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();
    scene->Add<Transform>(e).Position = vec3(0.0f);
    scene->Add<Name>(e, Name{.Value = "original"});

    const Unique<Scene> copy = scene->Clone();
    const Entity ce{.Index = e.Index, .Generation = e.Generation};

    // Edit the clone: the source is unaffected.
    copy->Get<Transform>(ce).Position = vec3(9.0f, 9.0f, 9.0f);
    copy->Get<Name>(ce).Value = "clone-edited";
    CHECK(scene->Get<Transform>(e).Position.x == doctest::Approx(0.0f));
    CHECK(scene->Get<Name>(e).Value == "original");

    // Edit the source: the clone is unaffected.
    scene->Get<Transform>(e).Position = vec3(-1.0f, -1.0f, -1.0f);
    CHECK(copy->Get<Transform>(ce).Position.x == doctest::Approx(9.0f));
    CHECK(copy->Get<Name>(ce).Value == "clone-edited");

    // Structural change to the source leaves the clone's entity count fixed.
    const usize cloneCount = copy->EntityCount();
    scene->DestroyEntity(e);
    CHECK(copy->EntityCount() == cloneCount);
    CHECK(copy->IsAlive(ce));
}
