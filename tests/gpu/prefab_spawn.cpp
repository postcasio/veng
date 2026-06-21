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
}

VE_REFLECT(Link, 0x7A9C1E55B0334401ULL)
VE_FIELD(Target)
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
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

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
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

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

TEST_CASE_FIXTURE(PrefabFixture,
                  "Intra-prefab entity references remap to the freshly spawned handles")
{
    // idx 0 links to idx 1; both are roots (no Parent), returned in order.
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Link{Entity{.Index = 1, .Generation = 0}})}});
    entities.push_back({{MakeComponent(Types, Name{"target"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

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
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

    REQUIRE(roots.size() == 1);
    CHECK(Stage->Get<Link>(roots[0]).Target.IsNull());
}

TEST_CASE_FIXTURE(PrefabFixture, "Spawning the same prefab twice yields independent copies")
{
    vector<Prefab::PrefabEntity> entities;
    entities.push_back({{MakeComponent(Types, Name{"x"})}});

    const Ref<Prefab> prefab = Prefab::Create(std::move(entities), {});
    const vector<Entity> first = prefab->SpawnInto(*Stage, *Assets);
    const vector<Entity> second = prefab->SpawnInto(*Stage, *Assets);

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
    const vector<Entity> roots = prefab->SpawnInto(*Stage, *Assets);

    REQUIRE(roots.size() == 1);
    const MeshRenderer& renderer = Stage->Get<MeshRenderer>(roots[0]);
    CHECK_FALSE(renderer.Mesh.Id().IsValid());
    CHECK_FALSE(renderer.Mesh.IsLoaded());
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
