// Mesh load test: cooks the mesh fixture pack in-process,
// mounts it, LoadSync<Mesh>s it through AssetManager, and checks the loaded
// mesh's vertex/index counts, canonical layout, resident material list +
// per-submesh material index, and GPU buffer sizes — the load-side proof for the
// mesh vertical slice.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "mesh loader: cook, mount, LoadSync, validate layout + submeshes")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "mesh_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_mesh.vengpack";

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

    std::filesystem::remove(outArchive);
}
