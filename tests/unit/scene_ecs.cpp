// ECS core unit cases: generational entities, type-erased sparse-set component
// pools, and the TypeRegistry lifecycle slice. Pure CPU — no Context, no Vulkan
// symbol touched.

#include <doctest/doctest.h>

#include <string>
#include <utility>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    // A trivially-relocatable component.
    struct Position
    {
        f32 X = 0.0f;
        f32 Y = 0.0f;
        f32 Z = 0.0f;
    };

    // A non-trivially-movable component (owns a heap string) — exercises the
    // pool's MoveConstruct/Destruct thunks on swap-and-pop.
    struct Label
    {
        std::string Text;
    };

    struct Velocity
    {
        f32 Dx = 0.0f;
        f32 Dy = 0.0f;
    };

    // A non-component leaf struct, registered to prove the registry is generic
    // (a component is just a reflected type a Scene pools).
    struct LeafColor
    {
        f32 R = 0.0f, G = 0.0f, B = 0.0f, A = 1.0f;
    };

    // Holds a pointer into its own storage that only its move constructor keeps right, the way a
    // small-buffer std::function or a node container does — so a byte copy breaks it.
    struct SelfAnchored
    {
        SelfAnchored() = default;
        SelfAnchored(const SelfAnchored& other) : Value(other.Value) {}
        SelfAnchored(SelfAnchored&& other) noexcept : Value(other.Value) {}
        SelfAnchored& operator=(const SelfAnchored& other)
        {
            Value = other.Value;
            return *this;
        }
        SelfAnchored& operator=(SelfAnchored&& other) noexcept
        {
            Value = other.Value;
            return *this;
        }
        ~SelfAnchored() = default;

        int Value = 0;
        const SelfAnchored* Self = this;
    };
}

VE_TYPE(::Position, 0x02C0484E07079107ULL);
VE_TYPE(::Label, 0xC8D3CFD3931A63D5ULL);
VE_TYPE(::Velocity, 0xE7ED834F2B1F7172ULL);
VE_TYPE(::LeafColor, 0x4BF560CC768E52A0ULL);
VE_TYPE(::SelfAnchored, 0x6528792901F4FAC6ULL);

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Position>("Position");
        registry.Register<Label>("Label");
        registry.Register<Velocity>("Velocity");
        registry.Register<LeafColor>("LeafColor");
        registry.Register<SelfAnchored>("SelfAnchored");
        return registry;
    }
}

// --- Entity lifecycle -------------------------------------------------------

TEST_CASE("Entity::Null is never alive and reports IsNull")
{
    CHECK(Entity::Null.IsNull());

    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    CHECK_FALSE(scene->IsAlive(Entity::Null));
}

TEST_CASE("CreateEntity yields a live entity; DestroyEntity makes it stale")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();
    CHECK(scene->IsAlive(e));
    CHECK(scene->EntityCount() == 1);

    scene->DestroyEntity(e);
    CHECK_FALSE(scene->IsAlive(e));
    CHECK(scene->EntityCount() == 0);
}

TEST_CASE("Index reuse bumps the generation so old handles go stale")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity first = scene->CreateEntity();
    scene->DestroyEntity(first);

    const Entity second = scene->CreateEntity();
    // The freed slot is reused, so the index repeats but the generation differs.
    CHECK(second.Index == first.Index);
    CHECK(second.Generation != first.Generation);

    CHECK(scene->IsAlive(second));
    CHECK_FALSE(scene->IsAlive(first));
}

TEST_CASE("CreateEntityAt restores the exact destroyed handle (slot + generation)")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity original = scene->CreateEntity();
    scene->Add<Position>(original, Position{.X = 7.0f});
    scene->DestroyEntity(original);
    CHECK_FALSE(scene->IsAlive(original));

    // Respawn at the exact handle: same index and the bumped generation, alive again.
    const Entity respawned = scene->CreateEntityAt(original);
    CHECK(respawned == original);
    CHECK(scene->IsAlive(original));
    CHECK(scene->EntityCount() == 1);
    // It is repopulated by the caller; CreateEntityAt adds no components.
    CHECK_FALSE(scene->Has<Position>(original));

    // The slot is no longer on the free list, so a fresh CreateEntity does not collide with it.
    const Entity other = scene->CreateEntity();
    CHECK(other.Index != original.Index);
    CHECK(scene->IsAlive(other));
}

TEST_CASE("CreateEntityAt grows the slot table, leaving skipped slots free")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A handle well past the current (empty) table grows it; the slots it skipped over
    // are unallocated, so they feed the next CreateEntity calls.
    const Entity target{.Index = 4, .Generation = 0};
    const Entity created = scene->CreateEntityAt(target);
    CHECK(created == target);
    CHECK(scene->IsAlive(target));

    // The four skipped slots are free, so the next CreateEntity lands inside [0, 4).
    const Entity next = scene->CreateEntity();
    CHECK(next.Index < target.Index);
    CHECK(scene->IsAlive(next));
}

// --- Component add / get / has / tryget / remove ----------------------------

TEST_CASE("Add/Get/Has/TryGet/Remove round-trip")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();

    CHECK_FALSE(scene->Has<Position>(e));
    CHECK(scene->TryGet<Position>(e) == nullptr);

    const auto& p = scene->Add<Position>(e, Position{.X = 1.0f, .Y = 2.0f, .Z = 3.0f});
    CHECK(scene->Has<Position>(e));
    CHECK(p.Y == doctest::Approx(2.0f));

    CHECK(scene->Get<Position>(e).Z == doctest::Approx(3.0f));
    CHECK(scene->TryGet<Position>(e) != nullptr);

    // A second component type on the same entity lives in its own pool.
    scene->Add<Velocity>(e, Velocity{.Dx = 0.5f, .Dy = -0.5f});
    CHECK(scene->Has<Velocity>(e));
    CHECK(scene->Get<Velocity>(e).Dx == doctest::Approx(0.5f));

    (void)scene->Remove<Position>(e);
    CHECK_FALSE(scene->Has<Position>(e));
    CHECK(scene->TryGet<Position>(e) == nullptr);
    // Velocity is untouched by removing Position.
    CHECK(scene->Has<Velocity>(e));

    // Removing an absent component is a safe no-op.
    (void)scene->Remove<Position>(e);
    CHECK_FALSE(scene->Has<Position>(e));
}

// --- Sparse-set swap-and-pop with a non-trivially-movable component ---------

TEST_CASE("Sparse-set swap-and-pop keeps survivors intact (non-trivial move)")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    constexpr int Count = 5;
    Entity entities[Count];
    for (int i = 0; i < Count; ++i)
    {
        entities[i] = scene->CreateEntity();
        scene->Add<Label>(entities[i], Label{std::string("label-") + std::to_string(i)});
    }

    // Remove from the middle: the tail (label-4) moves into the hole.
    (void)scene->Remove<Label>(entities[2]);

    CHECK_FALSE(scene->Has<Label>(entities[2]));
    CHECK(scene->Get<Label>(entities[0]).Text == "label-0");
    CHECK(scene->Get<Label>(entities[1]).Text == "label-1");
    CHECK(scene->Get<Label>(entities[3]).Text == "label-3");
    // The moved tail survives with its original value through MoveConstruct.
    CHECK(scene->Get<Label>(entities[4]).Text == "label-4");
}

TEST_CASE("A component that points into itself survives pool growth and compaction")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // Enough adds to reallocate the pool's storage several times.
    constexpr int Count = 100;
    vector<Entity> entities;
    for (int i = 0; i < Count; ++i)
    {
        entities.push_back(scene->CreateEntity());
        scene->Add<SelfAnchored>(entities.back()).Value = i;
    }
    (void)scene->Remove<SelfAnchored>(entities[10]);

    int anchored = 0;
    int valued = 0;
    for (int i = 0; i < Count; ++i)
    {
        if (const auto* component = scene->TryGet<SelfAnchored>(entities[i]))
        {
            anchored += component->Self == component ? 1 : 0;
            valued += component->Value == i ? 1 : 0;
        }
    }
    CHECK(anchored == Count - 1);
    CHECK(valued == Count - 1);
}

TEST_CASE("DestroyEntity removes the entity's components from every pool")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();

    scene->Add<Position>(a, Position{.X = 1, .Y = 1, .Z = 1});
    scene->Add<Label>(a, Label{"a-label"});
    scene->Add<Position>(b, Position{.X = 2, .Y = 2, .Z = 2});

    scene->DestroyEntity(a);

    // b's components survive a's destruction; the recycled index does not
    // resurrect a's components.
    const Entity reused = scene->CreateEntity();
    CHECK(reused.Index == a.Index);
    CHECK_FALSE(scene->Has<Position>(reused));
    CHECK_FALSE(scene->Has<Label>(reused));

    CHECK(scene->Has<Position>(b));
    CHECK(scene->Get<Position>(b).X == doctest::Approx(2.0f));
}

// --- TypeRegistry -----------------------------------------------------------

TEST_CASE("TypeRegistry Register/IdOf/Info round-trip")
{
    const TypeRegistry registry = MakeRegistry();

    CHECK(registry.Count() == 5);
    CHECK(registry.IsRegistered(registry.IdOf<Position>()));

    const TypeInfo& info = registry.Info(registry.IdOf<Position>());
    CHECK(info.Name == "Position");
    CHECK(info.Size == sizeof(Position));
    CHECK(info.Align == alignof(Position));
    CHECK(info.Id == registry.IdOf<Position>());
    CHECK(info.DefaultConstruct != nullptr);
    CHECK(info.Destruct != nullptr);
    CHECK(info.MoveConstruct != nullptr);
}

TEST_CASE(
    "Registration numbers types densely, and each registry's scenes resolve by their own numbering")
{
    TypeRegistry first;
    first.Register<Position>("Position");
    first.Register<Label>("Label");
    TypeRegistry second;
    second.Register<Label>("Label");
    second.Register<Position>("Position");

    CHECK(first.Info(first.IdOf<Position>()).Ordinal == 0);
    CHECK(first.Info(first.IdOf<Label>()).Ordinal == 1);
    CHECK(second.OrdinalOf<Label>() == 0);
    CHECK(second.OrdinalOf<Position>() == 1);
    CHECK(second.OrdinalOf<Velocity>() == InvalidTypeOrdinal);

    const Unique<Scene> a = Scene::Create(first);
    const Unique<Scene> b = Scene::Create(second);
    const Entity ea = a->CreateEntity();
    const Entity eb = b->CreateEntity();
    a->Add<Position>(ea, Position{.X = 1.0f});
    a->Add<Label>(ea, Label{"a"});
    b->Add<Position>(eb, Position{.X = 2.0f});
    b->Add<Label>(eb, Label{"b"});

    // Alternating between the two numberings is what would expose a stale per-type cache.
    bool matched = true;
    for (int round = 0; round < 3; ++round)
    {
        matched = matched && a->Get<Position>(ea).X == 1.0f && b->Get<Position>(eb).X == 2.0f;
        matched = matched && a->Get<Label>(ea).Text == "a" && b->Get<Label>(eb).Text == "b";
    }
    CHECK(matched);

    // A type the registry never registered has no pool: absent, not an assert.
    CHECK_FALSE(b->Has<Velocity>(eb));
    CHECK(b->TryGet<Velocity>(eb) == nullptr);
    int velocities = 0;
    for (auto [entity, velocity] : b->View<Velocity>())
    {
        (void)entity;
        (void)velocity;
        ++velocities;
    }
    CHECK(velocities == 0);
}

TEST_CASE("IdOf equals the authored literal and is constexpr-usable")
{
    const TypeRegistry registry;
    CHECK(registry.IdOf<Position>() == 0x02C0484E07079107ULL);

    // IdOf<T>() reads VengReflect<T>::Id — a compile-time constant.
    constexpr TypeId id = VengReflect<Position>::Id;
    static_assert(id == 0x02C0484E07079107ULL);
    CHECK(id == registry.IdOf<Position>());
}

TEST_CASE("Distinct types get distinct ids")
{
    const TypeRegistry registry;
    CHECK(registry.IdOf<Position>() != registry.IdOf<Velocity>());
    CHECK(registry.IdOf<Position>() != registry.IdOf<Label>());
    CHECK(registry.IdOf<Velocity>() != registry.IdOf<LeafColor>());
}

TEST_CASE("A non-component leaf struct registers like any other type")
{
    const TypeRegistry registry = MakeRegistry();
    // LeafColor is never Added to an entity, yet it registers and carries a
    // TypeId just like a component — the registry is generic over any type.
    CHECK(registry.IsRegistered(registry.IdOf<LeafColor>()));
    CHECK(registry.Info(registry.IdOf<LeafColor>()).Name == "LeafColor");
}
