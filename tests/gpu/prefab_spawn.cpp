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
#include <Veng/Asset/RawAsset.h>
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

    // A component carrying an opaque-bytes handle: the loader's dependency walk must
    // recognize AssetHandle<RawAsset> as a Raw dependency and load it.
    struct RawHolder
    {
        AssetHandle<RawAsset> Blob;
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

VE_REFLECT(::RawHolder, 0x42416B54EF245910ULL)
VE_FIELD(Blob)
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
            Types.Register<RawHolder>();
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

    // A well-formed cooked prefab blob: one entity carrying one component whose
    // reflection record is exactly `record` (produced by WriteFields).
    vector<u8> OneComponentPrefabBlob(u64 componentTypeId, const vector<u8>& record)
    {
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
    writer.Add(prefabId, AssetTypes::Prefab, blob);

    const MountHandle mount = Assets->MountMemory(writer.Build(), "truncated_prefab");

    const AssetResult<AssetHandle<Prefab>> result = Assets->LoadSync<Prefab>(prefabId);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Kind == AssetError::Corrupt);
    CHECK(result.error().Id == prefabId);
}

TEST_CASE_FIXTURE(PrefabFixture, "A prefab's embedded RawAsset handle loads as a dependency")
{
    const AssetId rawId{0x00A9F0FACADE0001ULL};
    const AssetId prefabId{0x00A9F0FACADE0002ULL};

    // Mount the opaque blob the prefab will reference, and take a handle carrying its id.
    ArchiveWriter rawWriter;
    rawWriter.Add(rawId, AssetTypes::Raw, vector<u8>{7, 8, 9});
    const MountHandle rawMount = Assets->MountMemory(rawWriter.Build(), "raw_dep");
    const AssetResult<AssetHandle<RawAsset>> raw = Assets->LoadSync<RawAsset>(rawId);
    REQUIRE(raw.has_value());

    RawHolder holder;
    holder.Blob = *raw;

    // Author a prefab whose RawHolder names that blob, and load it: the loader's
    // dependency walk must recognize AssetHandle<RawAsset> as a Raw dependency and
    // resolve it, rather than rejecting the field as an unknown asset type.
    ArchiveWriter prefabWriter;
    const Prefab::Component component = MakeComponent(Types, holder);
    prefabWriter.Add(prefabId, AssetTypes::Prefab,
                     OneComponentPrefabBlob(component.Type, component.Record));
    const MountHandle prefabMount = Assets->MountMemory(prefabWriter.Build(), "raw_prefab");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(prefabId);
    REQUIRE(loaded.has_value());

    // The spawned component resolves its handle to the resident opaque blob.
    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(roots.size() == 1);
    const RawHolder& spawned = Stage->Get<RawHolder>(roots[0]);
    CHECK(spawned.Blob.Id() == rawId);
    CHECK(spawned.Blob.IsLoaded());
}

namespace
{
    // One cooked entity for the blob builder: its components, and the prefab that is its body
    // (0 for an ordinary entity).
    struct BlobEntity
    {
        vector<Prefab::Component> Components;
        u64 NestedPrefab = 0;
    };

    // A well-formed cooked prefab blob over an arbitrary entity set — the bytes the cooker
    // emits, so a nesting test drives the loader's own decode and dependency fan-out rather
    // than hand-building a Prefab the manager could not resolve a nested id against.
    vector<u8> PrefabBlob(std::span<const BlobEntity> entities)
    {
        vector<CookedPrefabEntity> entityTable;
        vector<CookedPrefabComponent> componentTable;
        vector<u8> records;

        for (const BlobEntity& blobEntity : entities)
        {
            entityTable.push_back(
                CookedPrefabEntity{.FirstComponent = static_cast<u32>(componentTable.size()),
                                   .ComponentCount = static_cast<u32>(blobEntity.Components.size()),
                                   .NestedPrefab = blobEntity.NestedPrefab});

            for (const Prefab::Component& component : blobEntity.Components)
            {
                componentTable.push_back(
                    CookedPrefabComponent{.TypeId = component.Type,
                                          .RecordOffset = static_cast<u32>(records.size()),
                                          .RecordSize = static_cast<u32>(component.Record.size())});
                records.insert(records.end(), component.Record.begin(), component.Record.end());
            }
        }

        CookedPrefabHeader header;
        header.Version = CookedPrefabVersion;
        header.EntityCount = static_cast<u32>(entityTable.size());
        header.ComponentCount = static_cast<u32>(componentTable.size());
        header.RecordBytes = static_cast<u32>(records.size());

        vector<u8> blob;
        PushPod(blob, header);
        for (const CookedPrefabEntity& entry : entityTable)
        {
            PushPod(blob, entry);
        }
        for (const CookedPrefabComponent& entry : componentTable)
        {
            PushPod(blob, entry);
        }
        blob.insert(blob.end(), records.begin(), records.end());
        return blob;
    }

    constexpr AssetId ChildPrefabId{0x00C0FFEE00000001ULL};
    constexpr AssetId ParentPrefabId{0x00C0FFEE00000002ULL};
    constexpr AssetId GrandparentPrefabId{0x00C0FFEE00000003ULL};
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "A nesting entity is its expansion's root and keeps the body's hierarchy")
{
    // The body: a root "body" with one child "limb" under it.
    const BlobEntity child[] = {
        {.Components = {MakeComponent(Types, Name{"body"})}},
        {.Components = {MakeComponent(Types, Name{"limb"}),
                        MakeComponent(Types,
                                      Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}})}},
    };
    // The parent: an ordinary root, then an entity whose body is the child prefab.
    const BlobEntity parent[] = {
        {.Components = {MakeComponent(Types, Name{"scene"})}},
        {.NestedPrefab = ChildPrefabId.Value},
    };

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_prefabs");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;

    // Both of the parent's own entities are roots, and the nesting one *is* the body — no
    // container between them.
    REQUIRE(roots.size() == 2);
    CHECK(Stage->Get<Name>(roots[0]).Value == "scene");
    CHECK(Stage->Get<Name>(roots[1]).Value == "body");

    // The child prefab's own hierarchy survived becoming the nesting entity's.
    vector<Entity> children;
    Stage->ForEachChild(roots[1], [&](Entity entity) { children.push_back(entity); });
    REQUIRE(children.size() == 1);
    CHECK(Stage->Get<Name>(children[0]).Value == "limb");

    // Three authored entities, three spawned entities.
    CHECK(Stage->EntityCount() == 3);
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "A nesting entity's components add and replace whole components on the root")
{
    Transform placed;
    placed.Position = vec3{9.0f, 0.0f, 0.0f};

    // The body carries a Name and no Transform.
    const BlobEntity child[] = {{.Components = {MakeComponent(Types, Name{"body"})}}};
    // The nesting entity replaces the Name and adds a Transform.
    const BlobEntity parent[] = {
        {.Components = {MakeComponent(Types, Name{"placed"}), MakeComponent(Types, placed)},
         .NestedPrefab = ChildPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_overrides");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(roots.size() == 1);

    CHECK(Stage->Get<Name>(roots[0]).Value == "placed");
    CHECK(Stage->Get<Transform>(roots[0]).Position == vec3{9.0f, 0.0f, 0.0f});

    // The placement and the body are one entity: nothing hangs below it, and the spawn left no
    // container beside it.
    usize children = 0;
    Stage->ForEachChild(roots[0], [&](Entity) { ++children; });
    CHECK(children == 0);
    CHECK(Stage->EntityCount() == 1);
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "A prefab nesting a prefab that nests one composes onto one entity")
{
    Transform inner;
    inner.Position = vec3{1.0f, 0.0f, 0.0f};
    Transform middle;
    middle.Position = vec3{2.0f, 0.0f, 0.0f};

    // The innermost body says what the thing is; each level above it replaces one of its
    // components and leaves the rest standing.
    const BlobEntity child[] = {
        {.Components = {MakeComponent(Types, Name{"inner"}), MakeComponent(Types, inner),
                        MakeComponent(Types, Authority{.Tier = Tier::Local})}}};
    const BlobEntity parent[] = {
        {.Components = {MakeComponent(Types, middle)}, .NestedPrefab = ChildPrefabId.Value}};
    const BlobEntity grandparent[] = {{.Components = {MakeComponent(Types, Name{"outer"})},
                                       .NestedPrefab = ParentPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    writer.Add(GrandparentPrefabId, AssetTypes::Prefab, PrefabBlob(grandparent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_two_tiers");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(GrandparentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(roots.size() == 1);

    // One entity carries all three levels, each component from the outermost level that authored
    // it: the grandparent's name, the parent's transform, the child's authority.
    CHECK(Stage->EntityCount() == 1);
    CHECK(Stage->Get<Name>(roots[0]).Value == "outer");
    CHECK(Stage->Get<Transform>(roots[0]).Position == vec3{2.0f, 0.0f, 0.0f});
    CHECK(Stage->Get<Authority>(roots[0]).Tier == Tier::Local);
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "A nesting entity whose expansion is empty still carries its records")
{
    // Two ways an expansion materializes nothing: a prefab with no entities at all, and one whose
    // every entity a client-mode load skips as server-authoritative.
    const BlobEntity parent[] = {{.Components = {MakeComponent(Types, Name{"placed"})},
                                  .NestedPrefab = ChildPrefabId.Value}};

    SUBCASE("an empty prefab")
    {
        ArchiveWriter writer;
        writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(std::span<const BlobEntity>{}));
        writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
        const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_empty");

        const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
        REQUIRE(loaded.has_value());

        const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
        REQUIRE(roots.size() == 1);
        CHECK(Stage->Get<Name>(roots[0]).Value == "placed");
        CHECK(Stage->EntityCount() == 1);
    }

    SUBCASE("a body skipped whole by a client-mode load")
    {
        // No Authority is the authored default (Server), so the body's one entity is skipped.
        const BlobEntity child[] = {{.Components = {MakeComponent(Types, Name{"body"})}}};

        ArchiveWriter writer;
        writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
        writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
        const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_skipped_body");

        const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
        REQUIRE(loaded.has_value());

        const vector<Entity> roots =
            (*loaded)
                ->SpawnInto(*Stage, *Assets, Prefab::SpawnOptions{.SkipServerAuthoritative = true})
                .Roots;
        REQUIRE(roots.size() == 1);
        CHECK(Stage->Get<Name>(roots[0]).Value == "placed");
        CHECK(Stage->EntityCount() == 1);
    }
}

TEST_CASE_FIXTURE(PrefabFixture, "A Reference to a nesting entity resolves to the composed entity")
{
    const BlobEntity child[] = {{.Components = {MakeComponent(Types, Name{"body"})}}};
    // Entity 0 points at entity 1, which is the nesting entity — so the link must land on the
    // thing the nest produced, not on something standing in front of it.
    const BlobEntity parent[] = {
        {.Components = {MakeComponent(Types, Link{.Target = Entity{.Index = 1, .Generation = 0}})}},
        {.NestedPrefab = ChildPrefabId.Value},
    };

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_reference");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(roots.size() == 2);

    const Entity target = Stage->Get<Link>(roots[0]).Target;
    CHECK(target == roots[1]);
    CHECK(Stage->Get<Name>(target).Value == "body");
}

TEST_CASE_FIXTURE(PrefabFixture, "Spawning a nesting prefab twice yields independent copies")
{
    const BlobEntity child[] = {{.Components = {MakeComponent(Types, Name{"body"})}}};
    const BlobEntity parent[] = {{.NestedPrefab = ChildPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_twice");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> first = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    const vector<Entity> second = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(first[0] != second[0]);

    Stage->Get<Name>(first[0]).Value = "mutated";
    CHECK(Stage->Get<Name>(second[0]).Value == "body");
}

TEST_CASE_FIXTURE(PrefabFixture, "A spawned root's PrefabSource names the prefab that composed it")
{
    // A body with two roots: the first becomes the nesting entity, the second hangs under it.
    const BlobEntity child[] = {{.Components = {MakeComponent(Types, Name{"body"})}},
                                {.Components = {MakeComponent(Types, Name{"sibling"})}}};
    const BlobEntity parent[] = {{.NestedPrefab = ChildPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_provenance");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots = (*loaded)->SpawnInto(*Stage, *Assets).Roots;
    REQUIRE(roots.size() == 1);

    // The composed root is reproduced by the prefab whose overrides it carries — the outer one.
    CHECK(Stage->Get<Name>(roots[0]).Value == "body");
    CHECK(Stage->Get<PrefabSource>(roots[0]).Prefab == ParentPrefabId);

    // The expansion's other root the outer prefab did not compose onto keeps the body's own id.
    vector<Entity> children;
    Stage->ForEachChild(roots[0], [&](Entity entity) { children.push_back(entity); });
    REQUIRE(children.size() == 1);
    CHECK(Stage->Get<Name>(children[0]).Value == "sibling");
    CHECK(Stage->Get<PrefabSource>(children[0]).Prefab == ChildPrefabId);
}

TEST_CASE_FIXTURE(PrefabFixture, "A nested spawn's pending handles ride the parent's batch")
{
    MeshRenderer renderer;
    renderer.Source.SetActive(Types.IdOf<CubeShape>());

    const BlobEntity child[] = {{.Components = {MakeComponent(Types, renderer)}}};
    const BlobEntity parent[] = {{.NestedPrefab = ChildPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_residency");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    Prefab::SpawnResult spawned = (*loaded)->SpawnInto(*Stage, *Assets);

    // The recipe mesh the *child* built streams in async, and the parent's single batch is
    // what reports it.
    CHECK(spawned.Pending.TotalCount() == 1);
    CHECK_FALSE(spawned.Pending.IsResident());
    spawned.Pending.WaitResident(Tasks);
    CHECK(spawned.Pending.IsResident());
}

TEST_CASE_FIXTURE(PrefabFixture,
                  "SkipServerAuthoritative skips an authoritative entity inside a nested prefab")
{
    // "local" survives a client-mode load; "server" (no Authority — the authored default) does
    // not, and the nesting entity that carries neither is never itself skipped.
    const BlobEntity child[] = {
        {.Components = {MakeComponent(Types, Name{"local"}),
                        MakeComponent(Types, Authority{.Tier = Tier::Local})}},
        {.Components = {MakeComponent(Types, Name{"server"})}},
    };
    const BlobEntity parent[] = {{.NestedPrefab = ChildPrefabId.Value}};

    ArchiveWriter writer;
    writer.Add(ChildPrefabId, AssetTypes::Prefab, PrefabBlob(child));
    writer.Add(ParentPrefabId, AssetTypes::Prefab, PrefabBlob(parent));
    const MountHandle mount = Assets->MountMemory(writer.Build(), "nested_authority");

    const AssetResult<AssetHandle<Prefab>> loaded = Assets->LoadSync<Prefab>(ParentPrefabId);
    REQUIRE(loaded.has_value());

    const vector<Entity> roots =
        (*loaded)
            ->SpawnInto(*Stage, *Assets, Prefab::SpawnOptions{.SkipServerAuthoritative = true})
            .Roots;
    REQUIRE(roots.size() == 1);

    usize local = 0;
    usize server = 0;
    for (auto [entity, name] : Stage->View<Name>())
    {
        local += static_cast<usize>(name.Value == "local");
        server += static_cast<usize>(name.Value == "server");
    }
    CHECK(local == 1);
    CHECK(server == 0);
}
