// Mesh-socket lookup + attachment unit cases: pure CPU, no Vulkan. A Mesh built through the
// MeshInfo factory carries a socket list and empty GPU buffers, which is everything
// Mesh::FindSocket and AttachToSocket read. Covers the sorted binary search, the miss paths that
// must return rather than assert, and the composed world transform an attached child ends up at —
// including the orientation contract (a socket's local -Z is forward, +Y is up). The CPU reads —
// ParseCookedMeshSockets, AssetManager::ReadMeshSockets and ReadPrefabSockets — run against
// hand-built cooked blobs in a memory mount, through a manager whose Context is never initialized.
// The CPU skeleton reads — ReadSkeleton and ReadMeshSkeleton — run the same way, against the resident
// load of the same blob.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

#include "support/CookedSkeleton.h"

using namespace Veng;

namespace
{
    // A quarter turn about +Y: it aims a socket's forward (-Z) down -X and leaves up (+Y) alone.
    quat QuarterTurnY()
    {
        return glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    }

    // A device-free Mesh carrying only a name and a socket list, sorted by name as the cook emits.
    Ref<Mesh> SocketMesh()
    {
        return Mesh::Create(MeshInfo{
            .Name = "socketed",
            .Sockets =
                {
                    MeshSocket{.Name = "Mount_A", .Position = vec3(1.0f, 2.0f, 3.0f)},
                    MeshSocket{.Name = "Mount_B",
                               .Position = vec3(0.0f, 0.5f, 0.0f),
                               .Rotation = QuarterTurnY()},
                    MeshSocket{.Name = "Mount_C", .Position = vec3(-4.0f, 0.0f, 0.0f)},
                },
        });
    }

    void CheckVec(const vec3& actual, const vec3& expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-4));
        CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-4));
        CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-4));
    }

    // A scene wired with exactly the component types the socket path touches.
    struct SocketScene
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Assets;
        Unique<Scene> World;

        SocketScene()
        {
            Types.Register<Transform>("Transform");
            Types.Register<Hierarchy>("Hierarchy");
            Types.Register<MeshRenderer>("MeshRenderer");
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            World = Scene::Create(Types);
        }

        // An entity at `position` drawing a mesh carrying the fixture sockets.
        Entity SpawnSocketedMesh(const vec3& position)
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<MeshRenderer>(entity,
                                     MeshRenderer{.Mesh = Assets->Adopt<Mesh>(SocketMesh())});
            return entity;
        }
    };
}

TEST_CASE("Mesh::FindSocket resolves a name and misses cleanly")
{
    const Ref<Mesh> mesh = SocketMesh();
    REQUIRE(mesh->GetSockets().size() == 3);

    const MeshSocket* found = mesh->FindSocket("Mount_B");
    REQUIRE(found != nullptr);
    CHECK(found->Name == "Mount_B");
    CheckVec(found->Position, vec3(0.0f, 0.5f, 0.0f));

    // The binary search reaches both ends of the sorted list, and a name that is not there —
    // including one that is a prefix of a real socket — is a null, not an assert.
    CHECK(mesh->FindSocket("Mount_A") != nullptr);
    CHECK(mesh->FindSocket("Mount_C") != nullptr);
    CHECK(mesh->FindSocket("Mount") == nullptr);
    CHECK(mesh->FindSocket("Mount_D") == nullptr);
    CHECK(mesh->FindSocket("") == nullptr);

    // A mesh authored with no sockets answers every lookup with null.
    const Ref<Mesh> bare = Mesh::Create(MeshInfo{.Name = "bare"});
    CHECK(bare->GetSockets().empty());
    CHECK(bare->FindSocket("Mount_A") == nullptr);
}

TEST_CASE("AttachToSocket parents the child and places it at the socket's world transform")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(10.0f, 0.0f, 0.0f));

    const Entity child = fixture.World->CreateEntity();
    REQUIRE(AttachToSocket(*fixture.World, child, host, "Mount_A"));

    CHECK(fixture.World->GetParent(child) == host);

    // The child's own Transform is the socket's mesh-space transform, so its world matrix is the
    // host's world composed with the socket — the authored place on the model.
    const mat4 world = WorldMatrix(*fixture.World, child);
    CheckVec(vec3(world[3]), vec3(11.0f, 2.0f, 3.0f));

    // Plain parenting, so moving the host carries the child with it for free.
    fixture.World->Get<Transform>(host).Position = vec3(0.0f, 100.0f, 0.0f);
    CheckVec(vec3(WorldMatrix(*fixture.World, child)[3]), vec3(1.0f, 102.0f, 3.0f));
}

TEST_CASE("AttachToSocket applies the socket's orientation, not only its position")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(0.0f));

    const Entity child = fixture.World->CreateEntity();
    REQUIRE(AttachToSocket(*fixture.World, child, host, "Mount_B"));

    // A socket's local -Z is forward and +Y is up. Mount_B is a quarter turn about +Y, so the
    // attached child faces -X in world space and still points up along +Y.
    const mat4 world = WorldMatrix(*fixture.World, child);
    CheckVec(vec3(world * vec4(0.0f, 0.0f, -1.0f, 0.0f)), vec3(-1.0f, 0.0f, 0.0f));
    CheckVec(vec3(world * vec4(0.0f, 1.0f, 0.0f, 0.0f)), vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("AttachToSocket reports a miss instead of asserting")
{
    SocketScene fixture;
    const Entity host = fixture.SpawnSocketedMesh(vec3(0.0f));

    const Entity child = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(child, Transform{.Position = vec3(7.0f, 7.0f, 7.0f)});

    // A missing socket is a content error the caller reports; the child is left where it was.
    CHECK_FALSE(AttachToSocket(*fixture.World, child, host, "Mount_Missing"));
    CHECK(fixture.World->GetParent(child).IsNull());
    CheckVec(fixture.World->Get<Transform>(child).Position, vec3(7.0f, 7.0f, 7.0f));

    // So is an entity that draws nothing, and one whose mesh handle is not resident.
    const Entity bare = fixture.World->CreateEntity();
    CHECK_FALSE(AttachToSocket(*fixture.World, child, bare, "Mount_A"));
    CHECK(FindMeshSocket(*fixture.World, bare, "Mount_A") == nullptr);

    const Entity pending = fixture.World->CreateEntity();
    fixture.World->Add<MeshRenderer>(pending, MeshRenderer{});
    CHECK_FALSE(AttachToSocket(*fixture.World, child, pending, "Mount_A"));

    CHECK(FindMeshSocket(*fixture.World, host, "Mount_A") != nullptr);
    CHECK(FindMeshSocket(*fixture.World, Entity::Null, "Mount_A") == nullptr);
}

namespace
{
    template <typename T>
    void PushPod(vector<u8>& out, const T& value)
    {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    CookedMeshSocket CookSocket(const char* name, const vec3& position, const quat& rotation)
    {
        CookedMeshSocket socket;
        std::strncpy(socket.Name, name, ShaderNameCapacity - 1);
        socket.Position[0] = position.x;
        socket.Position[1] = position.y;
        socket.Position[2] = position.z;
        socket.Rotation[0] = rotation.x;
        socket.Rotation[1] = rotation.y;
        socket.Rotation[2] = rotation.z;
        socket.Rotation[3] = rotation.w;
        return socket;
    }

    // A cooked mesh blob up to and including its socket table. The attribute and submesh tables
    // are present (zeroed) so the socket table sits where a real cook puts it; no geometry
    // follows, which a socket read never reaches.
    vector<u8> MeshBlob(std::span<const CookedMeshSocket> sockets, const u64 skeletonId = 0)
    {
        CookedMeshHeader header;
        header.Version = CookedMeshVersion;
        header.SkeletonId = skeletonId;
        header.AttributeCount = 4;
        header.SubMeshCount = 2;
        header.SocketCount = static_cast<u32>(sockets.size());

        vector<u8> blob;
        PushPod(blob, header);
        for (u32 i = 0; i < header.AttributeCount; ++i)
        {
            PushPod(blob, CookedVertexAttribute{});
        }
        for (u32 i = 0; i < header.SubMeshCount; ++i)
        {
            PushPod(blob, CookedSubMesh{});
        }
        for (const CookedMeshSocket& socket : sockets)
        {
            PushPod(blob, socket);
        }
        return blob;
    }

    template <class T>
    Prefab::Component Record(const TypeRegistry& types, const T& value)
    {
        Prefab::Component component;
        component.Type = TypeIdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    // A MeshRenderer record naming a cooked mesh by id, as a cooked prefab carries it.
    Prefab::Component MeshRecord(const TypeRegistry& types, const AssetId mesh)
    {
        MeshRenderer renderer;
        Detail::RehydrateHandleField(&renderer.Mesh, mesh, nullptr);
        return Record(types, renderer);
    }

    // A cooked prefab blob over plain (non-nesting) entities.
    vector<u8> PrefabBlob(std::span<const vector<Prefab::Component>> entities)
    {
        vector<CookedPrefabEntity> entityTable;
        vector<CookedPrefabComponent> componentTable;
        vector<u8> records;
        for (const vector<Prefab::Component>& components : entities)
        {
            entityTable.push_back(
                CookedPrefabEntity{.FirstComponent = static_cast<u32>(componentTable.size()),
                                   .ComponentCount = static_cast<u32>(components.size())});
            for (const Prefab::Component& component : components)
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

    constexpr AssetId BaseMeshId{0x75A35293729549FCULL};
    constexpr AssetId LimbMeshId{0x675C5C32F30E55DDULL};
    constexpr AssetId TwoLevelPrefabId{0xC2A48FACCB520969ULL};

    // A manager over a Context that is never initialized: no instance, no device, no GPU work.
    struct HeadlessAssets
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Assets;

        HeadlessAssets()
        {
            RegisterBuiltinTypes(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
        }
    };
}

TEST_CASE("ParseCookedMeshSockets decodes every socket of a cooked table and rejects a short one")
{
    const CookedMeshSocket cooked[] = {
        CookSocket("Mount_A", vec3(1.0f, 2.0f, 3.0f), quat(1.0f, 0.0f, 0.0f, 0.0f)),
        CookSocket("Mount_B", vec3(0.0f, 0.5f, 0.0f), QuarterTurnY()),
        CookSocket("Mount_C", vec3(-4.0f, 0.0f, 0.0f), quat(1.0f, 0.0f, 0.0f, 0.0f)),
    };
    const vector<u8> blob = MeshBlob(cooked);

    const Result<vector<MeshSocket>> sockets = ParseCookedMeshSockets(blob);
    REQUIRE(sockets.has_value());
    REQUIRE(sockets->size() == 3);
    CHECK((*sockets)[0].Name == "Mount_A");
    CHECK((*sockets)[2].Name == "Mount_C");
    CheckVec((*sockets)[0].Position, vec3(1.0f, 2.0f, 3.0f));
    CheckVec((*sockets)[2].Position, vec3(-4.0f, 0.0f, 0.0f));

    // The cooked xyzw quaternion comes back as the same rotation: Mount_B faces -X.
    CheckVec((*sockets)[1].Rotation * vec3(0.0f, 0.0f, -1.0f), vec3(-1.0f, 0.0f, 0.0f));
    CheckVec((*sockets)[1].Scale, vec3(1.0f));

    // One byte short of the socket table is a decode error, never a partial list.
    const vector<u8> truncated(blob.begin(), blob.end() - 1);
    CHECK_FALSE(ParseCookedMeshSockets(truncated).has_value());

    vector<u8> stale = blob;
    stale[offsetof(CookedMeshHeader, Version)] ^= 0xFF;
    CHECK_FALSE(ParseCookedMeshSockets(stale).has_value());
}

TEST_CASE("ReadMeshSockets reads a mounted mesh's sockets and leaves nothing resident")
{
    HeadlessAssets headless;
    const CookedMeshSocket cooked[] = {
        CookSocket("Grip", vec3(0.0f, 0.5f, 0.0f), QuarterTurnY()),
        CookSocket("Tip", vec3(0.0f, 0.0f, -1.0f), quat(1.0f, 0.0f, 0.0f, 0.0f)),
    };
    ArchiveWriter writer;
    writer.Add(LimbMeshId, AssetTypes::Mesh, MeshBlob(cooked));
    const MountHandle mount = headless.Assets->MountMemory(writer.Build(), "socket_meshes");

    const AssetResult<vector<MeshSocket>> sockets = headless.Assets->ReadMeshSockets(LimbMeshId);
    REQUIRE(sockets.has_value());
    REQUIRE(sockets->size() == 2);
    CHECK((*sockets)[0].Name == "Grip");
    CHECK((*sockets)[1].Name == "Tip");
    CHECK(headless.Assets->CachedEntry(LimbMeshId) == nullptr);

    const AssetResult<vector<MeshSocket>> missing = headless.Assets->ReadMeshSockets(BaseMeshId);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().Kind == AssetError::NotFound);
}

TEST_CASE("ReadPrefabSockets composes a child mesh's sockets into prefab-root space")
{
    HeadlessAssets headless;
    const TypeRegistry& types = headless.Types;

    const CookedMeshSocket baseSockets[] = {
        CookSocket("Top", vec3(0.0f, 0.0f, -2.0f), quat(1.0f, 0.0f, 0.0f, 0.0f)),
    };
    const CookedMeshSocket limbSockets[] = {
        CookSocket("Grip", vec3(0.0f, 0.5f, 0.0f), QuarterTurnY()),
        CookSocket("Tip", vec3(0.0f, 0.0f, -1.0f), quat(1.0f, 0.0f, 0.0f, 0.0f)),
    };

    // A root carrying its own placement (which root space leaves out) and a child offset and
    // turned a quarter about +Y under it. The child is authored first, and sorts last.
    const Transform childLocal{.Position = vec3(1.0f, 2.0f, 3.0f), .Rotation = QuarterTurnY()};
    const vector<Prefab::Component> entities[] = {
        {Record(types, Name{"limb"}), Record(types, childLocal),
         Record(types, Hierarchy{.Parent = Entity{.Index = 1, .Generation = 0}}),
         MeshRecord(types, LimbMeshId)},
        {Record(types, Name{"base"}), Record(types, Transform{.Position = vec3(10.0f, 0.0f, 0.0f)}),
         MeshRecord(types, BaseMeshId)},
    };

    ArchiveWriter writer;
    writer.Add(BaseMeshId, AssetTypes::Mesh, MeshBlob(baseSockets));
    writer.Add(LimbMeshId, AssetTypes::Mesh, MeshBlob(limbSockets));
    writer.Add(TwoLevelPrefabId, AssetTypes::Prefab, PrefabBlob(entities));
    const MountHandle mount = headless.Assets->MountMemory(writer.Build(), "socket_prefab");

    const AssetResult<vector<PrefabSocket>> sockets =
        ReadPrefabSockets(*headless.Assets, TwoLevelPrefabId);
    REQUIRE(sockets.has_value());
    REQUIRE(sockets->size() == 3);

    // Sorted by entity name, then socket name — not by authored order.
    const PrefabSocket& top = (*sockets)[0];
    const PrefabSocket& grip = (*sockets)[1];
    const PrefabSocket& tip = (*sockets)[2];
    CHECK(top.EntityName == "base");
    CHECK(top.Local.Name == "Top");
    CHECK(top.Mesh == BaseMeshId);
    CHECK(grip.EntityName == "limb");
    CHECK(grip.Local.Name == "Grip");
    CHECK(tip.EntityName == "limb");
    CHECK(tip.Local.Name == "Tip");
    CHECK(tip.Mesh == LimbMeshId);

    // On the root's own mesh, root space is mesh space: the root's placement is not applied.
    CheckVec(top.RootSpace.Position, vec3(0.0f, 0.0f, -2.0f));

    // Under the child, root space is childLocal composed with the socket, by hand.
    const auto expected = [&](const MeshSocket& socket)
    {
        return LocalMatrix(childLocal) *
               LocalMatrix(Transform{.Position = socket.Position, .Rotation = socket.Rotation});
    };
    for (const PrefabSocket* socket : {&tip, &grip})
    {
        const mat4 actual = LocalMatrix(socket->RootSpace);
        const mat4 composed = expected(socket->Local);
        CheckVec(vec3(actual[3]), vec3(composed[3]));
        CheckVec(vec3(actual * vec4(0.0f, 0.0f, -1.0f, 0.0f)),
                 vec3(composed * vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        CheckVec(vec3(actual * vec4(0.0f, 1.0f, 0.0f, 0.0f)),
                 vec3(composed * vec4(0.0f, 1.0f, 0.0f, 0.0f)));
    }

    // Concretely: the tip sits a unit ahead of the limb, and the child's quarter turn aims
    // it down -X; the grip's own quarter turn on top aims it down +Z.
    CheckVec(tip.RootSpace.Position, vec3(0.0f, 2.0f, 3.0f));
    CheckVec(grip.RootSpace.Rotation * vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 0.0f, 1.0f));

    // Nothing was loaded: neither the prefab nor either mesh is in the cache.
    CHECK(headless.Assets->CachedEntry(TwoLevelPrefabId) == nullptr);
    CHECK(headless.Assets->CachedEntry(BaseMeshId) == nullptr);
    CHECK(headless.Assets->CachedEntry(LimbMeshId) == nullptr);
}

namespace
{
    constexpr AssetId RigSkeletonId{0xE55A90A681976209ULL};
    constexpr AssetId SkinnedMeshId{0xDABED6E940731D56ULL};
    constexpr AssetId OrphanMeshId{0x68A19273BEBBB3D9ULL};

    bool SameMatrix(const mat4& a, const mat4& b)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (a[c][r] != doctest::Approx(b[c][r]).epsilon(1e-6))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void CheckSameSkeleton(const Skeleton& read, const Skeleton& resident)
    {
        CHECK(SameMatrix(read.GlobalInverse, resident.GlobalInverse));
        REQUIRE(read.GetBoneCount() == resident.GetBoneCount());
        for (usize i = 0; i < read.GetBoneCount(); ++i)
        {
            const Bone& a = read.Bones[i];
            const Bone& b = resident.Bones[i];
            CHECK(a.Name == b.Name);
            CHECK(a.Parent == b.Parent);
            CheckVec(a.LocalPosition, b.LocalPosition);
            CheckVec(a.LocalScale, b.LocalScale);
            CHECK(std::abs(glm::dot(a.LocalRotation, b.LocalRotation)) ==
                  doctest::Approx(1.0f).epsilon(1e-6));
            CHECK(SameMatrix(a.InverseBind, b.InverseBind));
        }
    }
}

TEST_CASE("ReadSkeleton decodes a mounted skeleton exactly as the resident load does")
{
    HeadlessAssets headless;
    Skeleton authored = TestSupport::MakeArmRig();
    authored.Bones[1].LocalRotation = glm::angleAxis(glm::radians(30.0f), vec3(1.0f, 0.0f, 0.0f));
    authored.Bones[2].LocalScale = vec3(1.0f, 2.0f, 0.5f);
    ArchiveWriter writer;
    writer.Add(RigSkeletonId, AssetTypes::Skeleton, TestSupport::CookSkeleton(authored));
    const MountHandle mount = headless.Assets->MountMemory(writer.Build(), "skeleton");

    const AssetResult<Skeleton> read = headless.Assets->ReadSkeleton(RigSkeletonId);
    REQUIRE(read.has_value());
    CHECK(headless.Assets->CachedEntry(RigSkeletonId) == nullptr);
    CHECK(read->FindBone("Hand") == 2);
    CHECK(read->FindBone("Missing") == -1);

    const AssetResult<AssetHandle<Skeleton>> resident =
        headless.Assets->LoadSync<Skeleton>(RigSkeletonId);
    REQUIRE(resident.has_value());
    CheckSameSkeleton(*read, *resident->Get());

    const AssetResult<Skeleton> wrongType = headless.Assets->ReadMeshSkeleton(RigSkeletonId);
    REQUIRE_FALSE(wrongType.has_value());
    CHECK(wrongType.error().Kind == AssetError::WrongType);
}

TEST_CASE("ReadMeshSkeleton follows a mesh's skeleton reference and reports a static mesh")
{
    HeadlessAssets headless;
    const Skeleton authored = TestSupport::MakeArmRig();
    ArchiveWriter writer;
    writer.Add(RigSkeletonId, AssetTypes::Skeleton, TestSupport::CookSkeleton(authored));
    writer.Add(SkinnedMeshId, AssetTypes::Mesh, MeshBlob({}, RigSkeletonId.Value));
    writer.Add(BaseMeshId, AssetTypes::Mesh, MeshBlob({}));
    writer.Add(OrphanMeshId, AssetTypes::Mesh, MeshBlob({}, LimbMeshId.Value));
    const MountHandle mount = headless.Assets->MountMemory(writer.Build(), "skinned_mesh");

    const AssetResult<Skeleton> read = headless.Assets->ReadMeshSkeleton(SkinnedMeshId);
    REQUIRE(read.has_value());
    CheckSameSkeleton(*read, authored);
    CHECK(headless.Assets->CachedEntry(SkinnedMeshId) == nullptr);
    CHECK(headless.Assets->CachedEntry(RigSkeletonId) == nullptr);

    const AssetResult<Skeleton> unskinned = headless.Assets->ReadMeshSkeleton(BaseMeshId);
    REQUIRE_FALSE(unskinned.has_value());
    CHECK(unskinned.error().Kind == AssetError::LoadFailed);

    const AssetResult<Skeleton> orphan = headless.Assets->ReadMeshSkeleton(OrphanMeshId);
    REQUIRE_FALSE(orphan.has_value());
    CHECK(orphan.error().Kind == AssetError::MissingDependency);
    CHECK(orphan.error().Id == LimbMeshId);
}
