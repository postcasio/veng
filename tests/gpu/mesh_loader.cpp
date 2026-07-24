// Mesh load test: cooks the mesh fixture pack in-process,
// mounts it, LoadSync<Mesh>s it through AssetManager, and checks the loaded
// mesh's vertex/index counts, canonical layout, resident material list +
// per-submesh material index, GPU buffer sizes, and socket table — the load-side proof for the
// mesh vertical slice, through to an entity attached at an authored socket.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

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

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
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
    REQUIRE(cooker.CookPack(fixtureDir / "socket_pack.json", outArchive).has_value());

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
