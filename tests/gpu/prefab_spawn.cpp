// Prefab::SpawnInto: the runtime spawn path the cooker feeds. The cooker side is
// covered by prefab_cook; this exercises the *spawn* — entity creation, component
// population from the reflection records, intra-prefab Entity-reference remapping
// to fresh handles, root selection, and double-spawn independence. Prefabs are
// hand-authored here (the WriteFields records the cooker would emit), so no cook
// is involved.
//
// It lives in the GPU band only because SpawnInto takes an AssetManager&, which
// requires a Context; the assertions touch no device. The manager is reached
// only for resident AssetHandle fields — every prefab here uses invalid ids, so
// it stays a no-op.

#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Reflection/Variant.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // A component carrying an intra-prefab entity reference (a Reference field):
    // the spawn path must remap its prefab-local index to the freshly spawned
    // handle.
    struct Link
    {
        Entity Target = Entity::Null;
    };

    // A variant alternative carrying a Reference field — the spawn-time Resolve
    // walk must descend into the active alternative and remap it.
    struct LinkShape
    {
        Entity Target = Entity::Null;
    };

    using ShapeVariant = Variant<LinkShape>;

    // A component whose payload is a variant: the Resolve Variant case must reach
    // the active alternative's Reference field.
    struct VariantHolder
    {
        ShapeVariant Shape;
    };
}

VE_REFLECT(::Link, 0x7A9C1E55B0334401ULL)
VE_FIELD(Target)
VE_REFLECT_END();

VE_REFLECT(::LinkShape, 0x75D92F4155F0CF1BULL)
VE_FIELD(Target)
VE_REFLECT_END();

VE_VARIANT(::ShapeVariant, 0x1099063A85F1BA3DULL);

VE_REFLECT(::VariantHolder, 0xEBD7E2BC29BE5CA3ULL)
VE_FIELD(Shape)
VE_REFLECT_END();

namespace
{
    // Encode a component the way the cooker would: its TypeId plus the WriteFields
    // record that populates it at spawn.
    template <class T>
    Prefab::Component MakeComponent(const TypeRegistry& registry, const T& value)
    {
        Prefab::Component component;
        component.Type = registry.IdOf<T>();
        WriteFields(component.Record, &value, registry.Info(component.Type), registry);
        return component;
    }

    // A fixture wiring the builtin + test types into the GPU fixture's registry,
    // then standing up a Scene and AssetManager over it.
    struct PrefabFixture : Veng::Test::GpuFixture
    {
        Unique<Scene> Stage;
        Unique<AssetManager> Assets;

        PrefabFixture()
        {
            RegisterBuiltinTypes(Types);
            Types.Register<Link>();
            Types.Register<VariantHolder>();
            Stage = Scene::Create(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE_FIXTURE(PrefabFixture, "SpawnInto populates components and returns the single root")
{
    Transform transform;
    transform.Position = vec3{1.0f, 2.0f, 3.0f};

    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"hero"}), MakeComponent(Types, transform)}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 1);
    CHECK(Stage->IsAlive(roots[0]));
    CHECK(Stage->Get<Name>(roots[0]).Value == "hero");
    CHECK(Stage->Get<Transform>(roots[0]).Position == vec3{1.0f, 2.0f, 3.0f});
}

TEST_CASE_FIXTURE(PrefabFixture, "Roots are entities with no parent link, in authoring order")
{
    // idx 0 root "a", idx 1 child of 0 "b", idx 2 root "c".
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"a"})}});
    entities.push_back(
        {{MakeComponent(Types, Name{"b"}),
          MakeComponent(Types, Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}})}});
    entities.push_back({{MakeComponent(Types, Name{"c"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 2);
    CHECK(Stage->Get<Name>(roots[0]).Value == "a");
    CHECK(Stage->Get<Name>(roots[1]).Value == "c");

    // The child's Hierarchy parent edge (a Reference) was remapped from prefab
    // index 0 to the freshly spawned root, and the spawn rebuilt the intrusive
    // links — so the parent's child list contains the child.
    bool sawChild = false;
    for (auto [entity, name, hierarchy] : Stage->View<Name, Hierarchy>())
    {
        if (name.Value == "b")
        {
            sawChild = true;
            CHECK(hierarchy.Parent == roots[0]);
        }
    }
    CHECK(sawChild);

    // The rebuilt links round-trip: roots[0]'s sole child is the spawned "b".
    vector<Entity> children;
    Stage->ForEachChild(roots[0], [&](Entity child) { children.push_back(child); });
    REQUIRE(children.size() == 1);
    CHECK(Stage->Get<Name>(children[0]).Value == "b");
}

TEST_CASE_FIXTURE(PrefabFixture, "Two children under one parent both survive the link rebuild")
{
    // idx 0 parent "p"; idx 1 "a" and idx 2 "b" both reference parent index 0.
    // Both children's Hierarchy.Parent edges are pre-set by ReadFields; the rebuild
    // must link both, not drop the first when the second is attached.
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"p"})}});
    entities.push_back(
        {{MakeComponent(Types, Name{"a"}),
          MakeComponent(Types, Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}})}});
    entities.push_back(
        {{MakeComponent(Types, Name{"b"}),
          MakeComponent(Types, Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 1);
    CHECK(Stage->Get<Name>(roots[0]).Value == "p");

    // The parent keeps both children, in authoring order.
    vector<Entity> children;
    Stage->ForEachChild(roots[0], [&](Entity child) { children.push_back(child); });
    REQUIRE(children.size() == 2);
    CHECK(Stage->Get<Name>(children[0]).Value == "a");
    CHECK(Stage->Get<Name>(children[1]).Value == "b");

    // Each child's parent edge points back at the spawned parent.
    CHECK(Stage->GetParent(children[0]) == roots[0]);
    CHECK(Stage->GetParent(children[1]) == roots[0]);
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "Intra-prefab entity references remap to the freshly spawned handles")
{
    // idx 0 links to idx 1; both are roots (no Parent), returned in order.
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Link{Entity{.Index = 1, .Generation = 0}})}});
    entities.push_back({{MakeComponent(Types, Name{"target"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 2);
    const Entity target = Stage->Get<Link>(roots[0]).Target;
    CHECK(target == roots[1]);
    CHECK(Stage->IsAlive(target));
    CHECK(Stage->Get<Name>(target).Value == "target");
}

TEST_CASE_FIXTURE(PrefabFixture, "A null entity reference stays null after spawn")
{
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Link{Entity::Null})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 1);
    CHECK(Stage->Get<Link>(roots[0]).Target.IsNull());
}

TEST_CASE_FIXTURE(PrefabFixture, "Spawning the same prefab twice yields independent copies")
{
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"x"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> first = prefab->SpawnInto(*Stage, *Assets).Roots;
    const vector<Entity> second = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(first[0] != second[0]);
    CHECK(Stage->IsAlive(first[0]));
    CHECK(Stage->IsAlive(second[0]));

    // Mutating one copy leaves the other untouched.
    Stage->Get<Name>(first[0]).Value = "mutated";
    CHECK(Stage->Get<Name>(second[0]).Value == "x");
}

TEST_CASE_FIXTURE(PrefabFixture, "An embedded AssetHandle with an invalid id stays empty")
{
    // A default MeshRenderer carries an invalid (no-asset) mesh id — the resolve
    // leaves it empty rather than asking the manager for a resident entry.
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, MeshRenderer{})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 1);
    const MeshRenderer& renderer = Stage->Get<MeshRenderer>(roots[0]);
    CHECK_FALSE(renderer.Mesh.Id().IsValid());
    CHECK_FALSE(renderer.Mesh.IsLoaded());
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "Resolve descends into a variant's active alternative and remaps it")
{
    // idx 0 holds a variant whose active alternative references idx 1; the spawn
    // Resolve walk must reach inside the variant to remap the prefab-local index.
    VariantHolder holder;
    static_cast<LinkShape*>(holder.Shape.SetActive(Types.IdOf<LinkShape>()))->Target =
        Entity{.Index = 1, .Generation = 0};

    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, holder)}});
    entities.push_back({{MakeComponent(Types, Name{"target"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets).Roots;

    REQUIRE(roots.size() == 2);
    const VariantHolder& spawned = Stage->Get<VariantHolder>(roots[0]);
    REQUIRE(spawned.Shape.ActiveType() == Types.IdOf<LinkShape>());
    const Entity target = static_cast<const LinkShape*>(spawned.Shape.ActivePtr())->Target;
    CHECK(target == roots[1]);
    CHECK(Stage->Get<Name>(target).Value == "target");
}

namespace
{
    // Append a u32 in host byte order — the reflection record's own encoding
    // (Serialize.cpp), so a hand-built record matches the bytes the loader feeds
    // to ReadFields.
    void PushU32(vector<u8>& out, u32 value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    template <class T>
    void PushPod(vector<u8>& out, const T& value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    // A cooked prefab blob with one entity carrying one component whose record is
    // a *truncated* reflection record: it claims one field with a name length that
    // runs past the record's bytes. Every loader structural range check passes
    // (the record blob is exactly RecordSize bytes, RecordOffset + RecordSize ==
    // RecordBytes, the cooked blob is fully sized) — only ReadFields meets the
    // truncation, so the loader surfaces it as Corrupt rather than aborting.
    vector<u8> TruncatedPrefabBlob(u64 componentTypeId)
    {
        // The malformed record: recordCount=1, then a field name length of 64 with
        // no name bytes following — ReadFieldsInner's "truncated field name" guard.
        vector<u8> record;
        PushU32(record, 1);  // one field record
        PushU32(record, 64); // name length far past the record's end

        CookedPrefabHeader header;
        header.Version = CookedPrefabVersion;
        header.EntityCount = 1;
        header.ComponentCount = 1;
        header.RecordBytes = static_cast<u32>(record.size());

        const CookedPrefabEntity entity{.FirstComponent = 0, .ComponentCount = 1};
        const CookedPrefabComponent component{.TypeId = componentTypeId,
                                              .RecordOffset = 0,
                                              .RecordSize = static_cast<u32>(record.size())};

        vector<u8> blob;
        PushPod(blob, header);
        PushPod(blob, entity);
        PushPod(blob, component);
        blob.insert(blob.end(), record.begin(), record.end());
        return blob;
    }
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "A truncated cooked prefab record loads as AssetError::Corrupt, not an abort")
{
    const AssetId prefabId{0x5111A2C033B47ED9ULL};

    ArchiveWriter writer;
    const vector<u8> blob = TruncatedPrefabBlob(Types.IdOf<Transform>());
    writer.Add(prefabId, AssetType::Prefab, blob);

    const MountHandle mount = Assets->MountMemory(writer.Build(), "truncated_prefab");

    const AssetResult<AssetHandle<Prefab>> result = Assets->LoadSync<Prefab>(prefabId);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Kind == AssetError::Corrupt);
    CHECK(result.error().Id == prefabId);
}
