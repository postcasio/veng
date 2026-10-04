// Mesh load test: cooks the mesh fixture pack in-process,
// mounts it, LoadSync<Mesh>s it through AssetManager, and checks the loaded
// mesh's vertex/index counts, canonical layout, resident material list +
// per-submesh material index, GPU buffer sizes, and socket table — the load-side proof for the
// mesh vertical slice, through to an entity attached at an authored socket. The CPU socket reads
// are checked against the same cooked meshes made resident and a prefab of them spawned.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"
#include "support/TestCook.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "mesh loader: cook, mount, LoadSync, validate layout + submeshes")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "mesh_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_mesh.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = Veng::TestSupport::CookCached(cooker, packJson, outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Mesh>> handle = assets.LoadSync<Mesh>(AssetId{0xBB9});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Mesh& mesh = *handle->Get();

    CHECK(mesh.GetIndexType() == IndexType::U32);
    CHECK(mesh.GetIndexCount() == 36);

    // Layout matches the engine's canonical vertex layout.
    const VertexBufferLayout& layout = mesh.GetLayout();
    const VertexBufferLayout canonical = Mesh::CanonicalLayout();
    CHECK(layout.GetStride() == canonical.GetStride());
    REQUIRE(layout.GetElements().size() == canonical.GetElements().size());
    for (usize i = 0; i < layout.GetElements().size(); ++i)
    {
        CHECK(layout.GetElements()[i].Type == canonical.GetElements()[i].Type);
        CHECK(layout.GetElements()[i].Offset == canonical.GetElements()[i].Offset);
    }

    // Submesh table: one submesh over the whole index buffer, indexing the mesh's
    // resident material list (cube.mesh.json's "materials": { "0": 1003 }). The
    // loader eager-resolves id 1003 into one material instance the mesh owns.
    const std::span<const SubMesh> subMeshes = mesh.GetSubMeshes();
    REQUIRE(subMeshes.size() == 1);
    CHECK(subMeshes[0].IndexOffset == 0);
    CHECK(subMeshes[0].IndexCount == 36);
    REQUIRE(subMeshes[0].MaterialIndex != SubMesh::NoMaterial);

    const std::span<const AssetHandle<MaterialInstance>> materials = mesh.GetMaterials();
    REQUIRE(materials.size() == 1);
    REQUIRE(subMeshes[0].MaterialIndex < materials.size());
    CHECK(materials[subMeshes[0].MaterialIndex].IsLoaded());

    // GPU buffers sized to the cooked geometry (24 vertices * 48 bytes, 36 u32
    // indices) — consistent with the typed-buffer roundtrip cases' sanity checks.
    REQUIRE(mesh.GetVertexBuffer() != nullptr);
    REQUIRE(mesh.GetIndexBuffer().GetBuffer() != nullptr);
    CHECK(mesh.GetVertexBuffer()->GetSize() == static_cast<u64>(24) * 48);
    CHECK(mesh.GetIndexCount() == 36);
    CHECK(mesh.GetIndexBuffer().GetBuffer()->GetSize() == static_cast<u64>(36) * sizeof(u32));

    // cube.obj carries no named nodes, so the cooked mesh carries no sockets.
    CHECK(mesh.GetSockets().empty());

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "mesh loader: an authored socket round-trips into an attached entity's world pose")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_sockets.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "socket_pack.json", outArchive)
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Mesh>> handle = assets.LoadSync<Mesh>(AssetId{0x2D11});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Mesh& mesh = *handle->Get();
    REQUIRE(mesh.GetSockets().size() == 4);
    REQUIRE(mesh.FindSocket("Mount_C") != nullptr);
    CHECK(mesh.FindSocket("Body") == nullptr);

    // Cook -> load -> FindSocket -> AttachToSocket -> WorldMatrix: the attached child lands at
    // the transform the model authored, lifted through the host entity's own placement.
    TypeRegistry sceneTypes;
    sceneTypes.Register<Transform>("Transform");
    sceneTypes.Register<Hierarchy>("Hierarchy");
    sceneTypes.Register<MeshRenderer>("MeshRenderer");
    const Unique<Scene> scene = Scene::Create(sceneTypes);

    const Entity host = scene->CreateEntity();
    scene->Add<Transform>(host, Transform{.Position = vec3(0.0f, 1.0f, 0.0f)});
    scene->Add<MeshRenderer>(host, MeshRenderer{.Mesh = *handle});

    const Entity child = scene->CreateEntity();
    REQUIRE(AttachToSocket(*scene, child, host, "Mount_C"));
    CHECK(scene->GetParent(child) == host);

    // sockets.gltf puts Mount_C at local (2, 0, 0) under a parent translated (0, 5, 0) and turned
    // a quarter turn about +Y, which composes to (0, 5, -2) in mesh space.
    const mat4 world = WorldMatrix(*scene, child);
    CHECK(world[3].x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(world[3].y == doctest::Approx(6.0f).epsilon(1e-4));
    CHECK(world[3].z == doctest::Approx(-2.0f).epsilon(1e-4));

    // The socket's forward (-Z) rides through the attachment: the parent's quarter turn aims it
    // down -X in world space.
    const vec4 forward = world * vec4(0.0f, 0.0f, -1.0f, 0.0f);
    CHECK(forward.x == doctest::Approx(-1.0f).epsilon(1e-4));
    CHECK(forward.y == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(forward.z == doctest::Approx(0.0f).epsilon(1e-4));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "mesh loader: a version-mismatched cooked mesh loads as AssetError::Corrupt")
{
    const AssetId meshId{0x0000000000002D21ULL};

    CookedMeshHeader header{};
    header.Version = CookedMeshVersion + 1; // stale/foreign
    header.VertexStride = 48;
    header.IndexType = 1;
    header.AttributeCount = 4;

    vector<u8> blob(sizeof(header));
    std::memcpy(blob.data(), &header, sizeof(header));

    AssetManager assets(Context, Tasks, Types);
    ArchiveWriter writer;
    writer.Add(meshId, AssetTypes::Mesh, blob);
    const MountHandle mount = assets.MountMemory(writer.Build(), "stale_mesh");

    const AssetResult<AssetHandle<Mesh>> result = assets.LoadSync<Mesh>(meshId);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Kind == AssetError::Corrupt);
    CHECK(result.error().Id == meshId);
}

namespace
{
    template <class T>
    Prefab::Component SocketRecord(const TypeRegistry& types, const T& value)
    {
        Prefab::Component component;
        component.Type = TypeIdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    Prefab::Component SocketMeshRecord(const TypeRegistry& types, const AssetId mesh)
    {
        MeshRenderer renderer;
        Detail::RehydrateHandleField(&renderer.Mesh, mesh, nullptr);
        return SocketRecord(types, renderer);
    }

    struct SocketBlobEntity
    {
        vector<Prefab::Component> Components;
        u64 NestedPrefab = 0;
    };

    template <typename T>
    void AppendPod(vector<u8>& out, const T& value)
    {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    vector<u8> SocketPrefabBlob(std::span<const SocketBlobEntity> entities)
    {
        vector<CookedPrefabEntity> entityTable;
        vector<CookedPrefabComponent> componentTable;
        vector<u8> records;
        for (const SocketBlobEntity& entity : entities)
        {
            entityTable.push_back(
                CookedPrefabEntity{.FirstComponent = static_cast<u32>(componentTable.size()),
                                   .ComponentCount = static_cast<u32>(entity.Components.size()),
                                   .NestedPrefab = entity.NestedPrefab});
            for (const Prefab::Component& component : entity.Components)
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
        AppendPod(blob, header);
        for (const CookedPrefabEntity& entry : entityTable)
        {
            AppendPod(blob, entry);
        }
        for (const CookedPrefabComponent& entry : componentTable)
        {
            AppendPod(blob, entry);
        }
        blob.insert(blob.end(), records.begin(), records.end());
        return blob;
    }

    constexpr AssetId SocketedMeshId{0x2D11};
    constexpr AssetId ScaledSocketedMeshId{0x2D12};
    constexpr AssetId ArmPrefabId{0x40B9BAEDA9D5E863ULL};
    constexpr AssetId RigPrefabId{0x0E754C21E146904EULL};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "mesh loader: CPU socket reads agree with the resident meshes and a spawn")
{
    RegisterBuiltinTypes(Types);

    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_socket_reads.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "socket_pack.json", outArchive)
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // An arm (the scaled mesh) with a hand under it, offset and turned; and a rig whose base
    // carries its own placement and nests the arm under it with an overriding Transform.
    const quat quarterY = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    const quat quarterX = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
    const quat eighthZ = glm::angleAxis(glm::radians(45.0f), vec3(0.0f, 0.0f, 1.0f));
    const SocketBlobEntity arm[] = {
        {.Components = {SocketRecord(Types, Name{"arm"}),
                        SocketRecord(Types, Transform{.Position = vec3(0.0f, 1.0f, 0.0f)}),
                        SocketMeshRecord(Types, ScaledSocketedMeshId)}},
        {.Components = {SocketRecord(Types, Name{"hand"}),
                        SocketRecord(Types,
                                     Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}}),
                        SocketRecord(Types, Transform{.Position = vec3(0.0f, 0.0f, -3.0f),
                                                      .Rotation = quarterX}),
                        SocketMeshRecord(Types, SocketedMeshId)}},
    };
    const SocketBlobEntity rig[] = {
        {.Components = {SocketRecord(Types, Name{"base"}),
                        SocketRecord(Types, Transform{.Position = vec3(5.0f, 0.0f, 0.0f),
                                                      .Rotation = quarterY}),
                        SocketMeshRecord(Types, SocketedMeshId)}},
        {.Components = {SocketRecord(Types,
                                     Hierarchy{.Parent = Entity{.Index = 0, .Generation = 0}}),
                        SocketRecord(Types, Transform{.Position = vec3(2.0f, 0.0f, 0.0f),
                                                      .Rotation = eighthZ})},
         .NestedPrefab = ArmPrefabId.Value},
    };
    ArchiveWriter writer;
    writer.Add(ArmPrefabId, AssetTypes::Prefab, SocketPrefabBlob(arm));
    writer.Add(RigPrefabId, AssetTypes::Prefab, SocketPrefabBlob(rig));
    const MountHandle mount = assets.MountMemory(writer.Build(), "socket_rig");

    // Read before anything loads, and prove the read made nothing resident.
    const AssetResult<vector<PrefabSocket>> read = ReadPrefabSockets(assets, RigPrefabId);
    REQUIRE(read.has_value());
    CHECK(read->size() == 12);
    CHECK(assets.CachedEntry(RigPrefabId) == nullptr);
    CHECK(assets.CachedEntry(SocketedMeshId) == nullptr);

    // The mesh-level read is the resident mesh's socket table.
    const AssetResult<vector<MeshSocket>> meshRead = assets.ReadMeshSockets(ScaledSocketedMeshId);
    const AssetResult<AssetHandle<Mesh>> scaled = assets.LoadSync<Mesh>(ScaledSocketedMeshId);
    REQUIRE(meshRead.has_value());
    REQUIRE(scaled.has_value());
    const std::span<const MeshSocket> resident = (*scaled)->GetSockets();
    REQUIRE(meshRead->size() == resident.size());
    for (usize i = 0; i < resident.size(); ++i)
    {
        CHECK((*meshRead)[i].Name == resident[i].Name);
        CHECK((*meshRead)[i].Position == resident[i].Position);
        CHECK((*meshRead)[i].Rotation == resident[i].Rotation);
        CHECK((*meshRead)[i].Scale == resident[i].Scale);
    }

    // Spawn the rig with its meshes resident. An entity attached at each read socket lands at the
    // root's world transform composed with the read's root-space transform.
    const AssetResult<AssetHandle<Prefab>> loaded = assets.LoadSync<Prefab>(RigPrefabId);
    REQUIRE(loaded.has_value());
    const Unique<Scene> scene = Scene::Create(Types);
    const vector<Entity> roots = (*loaded)->SpawnInto(*scene, assets).Roots;
    REQUIRE(roots.size() == 1);
    const mat4 rootWorld = WorldMatrix(*scene, roots[0]);

    map<string, Entity> byName;
    for (auto [entity, name] : scene->View<Name>())
    {
        byName[name.Value] = entity;
    }

    f32 worstError = 0.0f;
    for (const PrefabSocket& socket : *read)
    {
        REQUIRE(byName.contains(socket.EntityName));
        const Entity child = scene->CreateEntity();
        REQUIRE(AttachToSocket(*scene, child, byName[socket.EntityName], socket.Local.Name));

        const mat4 world = WorldMatrix(*scene, child);
        const mat4 predicted = rootWorld * LocalMatrix(socket.RootSpace);
        for (int column = 0; column < 4; ++column)
        {
            worstError =
                std::max(worstError, glm::length(vec4(world[column]) - vec4(predicted[column])));
        }
    }
    CHECK(worstError < 1e-4f);

    std::filesystem::remove(outArchive);
}
