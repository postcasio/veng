// Mesh cook test (planset-5 plan 07): cooks a fixture mesh pack through
// libveng_cook and checks the resulting CookedMeshHeader (assetformat),
// attribute descriptor, submesh table (incl. the material override), and buffer
// sizes against the fixture cube.obj + cube.mesh.json.

#include <cstring>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

TEST_CASE("Cooker: cooks a mesh pack into a CookedMeshHeader + buffers + submeshes")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "mesh_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_mesh.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3001});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Mesh);

    REQUIRE(entry->Blob.size() >= sizeof(CookedMeshHeader));

    CookedMeshHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.VertexStride == 48); // pos(12) + normal(12) + tangent(16) + uv(8)
    CHECK(header.VertexCount == 24);  // 4 per face * 6 faces (per-face normals/UVs)
    CHECK(header.IndexCount == 36);   // 2 triangles * 6 faces
    CHECK(header.IndexType == 1);     // U32
    CHECK(header.SubMeshCount == 1);
    CHECK(header.AttributeCount == 4);

    usize cursor = sizeof(CookedMeshHeader);

    // Attribute descriptor: the canonical position/normal/tangent/uv layout.
    const u32 expectedFormats[] = {9, 9, 10, 8}; // RGB32, RGB32, RGBA32, RG32
    const u32 expectedOffsets[] = {0, 12, 24, 40};
    for (u32 i = 0; i < header.AttributeCount; ++i)
    {
        CookedVertexAttribute attribute{};
        std::memcpy(&attribute, entry->Blob.data() + cursor + i * sizeof(CookedVertexAttribute), sizeof(attribute));
        CHECK(attribute.Format == expectedFormats[i]);
        CHECK(attribute.Offset == expectedOffsets[i]);
    }
    cursor += header.AttributeCount * sizeof(CookedVertexAttribute);

    // Submesh table: one submesh covering the whole index buffer, with the
    // material override from cube.mesh.json's "materials": { "0": 1003 }.
    CookedSubMesh subMesh{};
    std::memcpy(&subMesh, entry->Blob.data() + cursor, sizeof(subMesh));
    CHECK(subMesh.IndexOffset == 0);
    CHECK(subMesh.IndexCount == 36);
    CHECK(subMesh.MaterialId == 1003);
    cursor += header.SubMeshCount * sizeof(CookedSubMesh);

    // Buffers: header advertises sizes that exactly account for the blob tail.
    const usize vertexBytes = static_cast<usize>(header.VertexCount) * header.VertexStride;
    const usize indexBytes = static_cast<usize>(header.IndexCount) * sizeof(u32);
    CHECK(entry->Blob.size() == cursor + vertexBytes + indexBytes);

    std::filesystem::remove(outArchive);
}
