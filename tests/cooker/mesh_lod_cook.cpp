// Mesh level-of-detail cook: cooks the LOD fixture pack through libveng_cook and checks the
// level table the mesh importer appends. The fixture is a UV sphere whose two hemispheres are
// separate submeshes with their own materials, so the equator is a border between submeshes. The
// properties: generated levels draw strictly fewer triangles each, keep the submesh table and the
// sockets, and keep every submesh's border exactly where level 0 has it; an authored level replaces
// its generated slot, maps onto the base submeshes by material, and appends its vertices; a
// generated level below an authored one simplifies the authored model; a faceted mesh reduces only
// where a level may collapse its seams; a mesh with no "lod" cooks one level; an unmappable authored
// level and a skinned mesh are cook errors.

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <utility>

#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // A cooked mesh blob, decoded table by table.
    struct DecodedMesh
    {
        CookedMeshHeader Header{};
        vector<CookedSubMesh> SubMeshes;
        vector<CookedMeshSocket> Sockets;
        vector<vec3> Positions;
        vector<u32> Indices;
        vector<CookedMeshLod> Lods;
        vector<CookedMeshLodRange> Ranges;
        usize Bytes = 0;

        // A submesh's range at a level; level 0 is the submesh table.
        [[nodiscard]] std::pair<u32, u32> Range(const u32 level, const u32 subMesh) const
        {
            if (level == 0)
            {
                return {SubMeshes[subMesh].IndexOffset, SubMeshes[subMesh].IndexCount};
            }
            const CookedMeshLodRange& range = Ranges[(level - 1) * Header.SubMeshCount + subMesh];
            return {range.IndexOffset, range.IndexCount};
        }

        [[nodiscard]] u32 Triangles(const u32 level) const
        {
            u32 indices = 0;
            for (u32 s = 0; s < Header.SubMeshCount; ++s)
            {
                indices += Range(level, s).second;
            }
            return indices / 3;
        }
    };

    template <class T>
    void Read(const std::span<const u8> blob, usize& cursor, vector<T>& out, const usize count)
    {
        out.resize(count);
        std::memcpy(out.data(), blob.data() + cursor, count * sizeof(T));
        cursor += count * sizeof(T);
    }

    DecodedMesh Decode(const std::span<const u8> blob)
    {
        DecodedMesh mesh;
        mesh.Bytes = blob.size();
        std::memcpy(&mesh.Header, blob.data(), sizeof(mesh.Header));
        usize cursor =
            sizeof(mesh.Header) + mesh.Header.AttributeCount * sizeof(CookedVertexAttribute);
        Read(blob, cursor, mesh.SubMeshes, mesh.Header.SubMeshCount);
        Read(blob, cursor, mesh.Sockets, mesh.Header.SocketCount);
        for (u32 v = 0; v < mesh.Header.VertexCount; ++v)
        {
            vec3 position;
            std::memcpy(&position,
                        blob.data() + cursor + static_cast<usize>(v) * mesh.Header.VertexStride,
                        sizeof(position));
            mesh.Positions.push_back(position);
        }
        cursor += static_cast<usize>(mesh.Header.VertexCount) * mesh.Header.VertexStride;
        Read(blob, cursor, mesh.Indices, mesh.Header.IndexCount);
        Read(blob, cursor, mesh.Lods, mesh.Header.LodCount - 1);
        Read(blob, cursor, mesh.Ranges, (mesh.Header.LodCount - 1) * mesh.Header.SubMeshCount);
        CHECK(cursor == blob.size());
        return mesh;
    }

    // A position, quantized so the two sides of a seam compare equal.
    using Key = std::tuple<i32, i32, i32>;
    Key Quantize(const vec3& p)
    {
        return {static_cast<i32>(std::lround(p.x * 1e4f)),
                static_cast<i32>(std::lround(p.y * 1e4f)),
                static_cast<i32>(std::lround(p.z * 1e4f))};
    }

    // The border of a submesh at a level: every edge one triangle uses, as a pair of positions.
    std::set<std::pair<Key, Key>> Border(const DecodedMesh& mesh, const u32 level,
                                         const u32 subMesh)
    {
        const auto [offset, count] = mesh.Range(level, subMesh);
        std::map<std::pair<Key, Key>, u32> uses;
        for (u32 t = offset; t < offset + count; t += 3)
        {
            for (u32 e = 0; e < 3; ++e)
            {
                Key a = Quantize(mesh.Positions[mesh.Indices[t + e]]);
                Key b = Quantize(mesh.Positions[mesh.Indices[t + (e + 1) % 3]]);
                if (a == b)
                {
                    continue;
                }
                if (b < a)
                {
                    std::swap(a, b);
                }
                ++uses[{a, b}];
            }
        }
        std::set<std::pair<Key, Key>> border;
        for (const auto& [edge, n] : uses)
        {
            if (n == 1)
            {
                border.insert(edge);
            }
        }
        return border;
    }

    Result<ArchiveReader> CookLodPack(const char* pack, const char* archive)
    {
        const path outArchive = Veng::TestSupport::TempDir() / archive;
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const VoidResult cooked =
            cooker.CookPack(path(VENG_COOKER_TEST_FIXTURE_DIR) / pack, outArchive);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: generated levels of detail coarsen, keep the submeshes, and keep the seams")
{
    const Result<ArchiveReader> reader = CookLodPack("lod_pack.json", "veng_cooker_lod.vengpack");
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3A01});
    REQUIRE(entry.has_value());
    const DecodedMesh mesh = Decode(entry->Blob);

    CHECK(mesh.Header.Version == CookedMeshVersion);
    REQUIRE(mesh.Header.LodCount >= 3);
    REQUIRE(mesh.Header.SubMeshCount == 2);
    CHECK(mesh.Header.BaseVertexCount == mesh.Header.VertexCount);

    // Sockets are the mesh's, not a level's: the one empty node survives the levels being added.
    REQUIRE(mesh.Header.SocketCount == 1);
    CHECK(std::strcmp(mesh.Sockets[0].Name, "Mount") == 0);

    f32 previousSize = std::numeric_limits<f32>::infinity();
    for (u32 level = 1; level < mesh.Header.LodCount; ++level)
    {
        CHECK(mesh.Triangles(level) < mesh.Triangles(level - 1));
        const CookedMeshLod& lod = mesh.Lods[level - 1];
        CHECK(lod.ScreenSize > 0.0f);
        CHECK(lod.ScreenSize <= 1.0f);
        CHECK(lod.ScreenSize <= previousSize);
        previousSize = lod.ScreenSize;

        for (u32 s = 0; s < mesh.Header.SubMeshCount; ++s)
        {
            const auto [offset, count] = mesh.Range(level, s);
            REQUIRE(offset + count <= mesh.Header.IndexCount);
            CHECK(count > 0);
            // Each submesh's border — the equator its neighbour shares among it — is untouched, so
            // the two hemispheres meet without a crack at every level.
            CHECK(Border(mesh, level, s) == Border(mesh, 0, s));
        }
    }
}

TEST_CASE(
    "Cooker: an authored level replaces its slot and maps onto the base submeshes by material")
{
    const Result<ArchiveReader> reader =
        CookLodPack("lod_pack.json", "veng_cooker_lod_authored.vengpack");
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3A02});
    REQUIRE(entry.has_value());
    const DecodedMesh mesh = Decode(entry->Blob);

    REQUIRE(mesh.Header.LodCount == 3);
    REQUIRE(mesh.Header.SubMeshCount == 2);
    CHECK(mesh.Lods[1].ScreenSize == doctest::Approx(0.05f));
    CHECK(mesh.Lods[1].Error == 0.0f);

    // The authored model's vertices follow level 0's, and its ranges index only those.
    CHECK(mesh.Header.BaseVertexCount < mesh.Header.VertexCount);
    for (u32 s = 0; s < mesh.Header.SubMeshCount; ++s)
    {
        const auto [offset, count] = mesh.Range(2, s);
        // The coarse sphere's hemispheres: four rings of twelve quads each.
        CHECK(count == 4 * 12 * 6);
        u32 lowest = ~0u;
        for (u32 i = offset; i < offset + count; ++i)
        {
            lowest = std::min(lowest, mesh.Indices[i]);
        }
        CHECK(lowest >= mesh.Header.BaseVertexCount);

        // "Top" maps onto the upper hemisphere and "Bottom" onto the lower, whatever the order the
        // model lists them in.
        const f32 side = s == 0 ? 1.0f : -1.0f;
        f32 sum = 0.0f;
        for (u32 i = offset; i < offset + count; ++i)
        {
            sum += mesh.Positions[mesh.Indices[i]].y;
        }
        CHECK(sum * side > 0.0f);
    }
}

TEST_CASE("Cooker: a generated level below an authored one simplifies the authored model")
{
    const Result<ArchiveReader> reader =
        CookLodPack("lod_pack.json", "veng_cooker_lod_authored_seed.vengpack");
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3A04});
    REQUIRE(entry.has_value());
    const DecodedMesh mesh = Decode(entry->Blob);

    REQUIRE(mesh.Header.LodCount == 3);
    CHECK(mesh.Lods[0].Error == 0.0f);
    CHECK(mesh.Triangles(2) < mesh.Triangles(1));

    // Every index the generated level draws addresses the authored model's appended vertices, so
    // it was built from that model rather than from level 0.
    u32 lowest = ~0u;
    for (u32 s = 0; s < mesh.Header.SubMeshCount; ++s)
    {
        const auto [offset, count] = mesh.Range(2, s);
        CHECK(count > 0);
        for (u32 i = offset; i < offset + count; ++i)
        {
            lowest = std::min(lowest, mesh.Indices[i]);
        }
        // The seam the authored hemispheres share stays where the authored model put it.
        CHECK(Border(mesh, 2, s) == Border(mesh, 1, s));
    }
    CHECK(lowest >= mesh.Header.BaseVertexCount);
}

TEST_CASE("Cooker: a faceted mesh reduces only when its level may collapse its seams")
{
    // Every corner of a flat-shaded sphere is a position split once per face meeting there, which
    // the simplifier keeps fixed unless a level lets it collapse across those breaks.
    const Result<ArchiveReader> reader =
        CookLodPack("lod_pack.json", "veng_cooker_lod_faceted.vengpack");
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> plain = reader->Find(AssetId{0x3A05});
    const optional<ArchiveEntry> collapsed = reader->Find(AssetId{0x3A06});
    REQUIRE(plain.has_value());
    REQUIRE(collapsed.has_value());

    const DecodedMesh held = Decode(plain->Blob);
    const u32 full = held.Triangles(0);
    if (held.Header.LodCount > 1)
    {
        CHECK(held.Triangles(1) * 2 > full);
    }

    const DecodedMesh reduced = Decode(collapsed->Blob);
    REQUIRE(reduced.Header.LodCount == 2);
    CHECK(reduced.Triangles(1) * 2 < full);
}

TEST_CASE("Cooker: a mesh with no 'lod' key cooks a single level")
{
    const Result<ArchiveReader> reader =
        CookLodPack("lod_pack.json", "veng_cooker_lod_none.vengpack");
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3A03});
    REQUIRE(entry.has_value());
    const DecodedMesh mesh = Decode(entry->Blob);
    CHECK(mesh.Header.LodCount == 1);
    CHECK(mesh.Header.BaseVertexCount == mesh.Header.VertexCount);
}

TEST_CASE("Cooker: an authored level whose material matches no base submesh is a located error")
{
    const Result<ArchiveReader> reader =
        CookLodPack("lod_unmapped_pack.json", "veng_cooker_lod_unmapped.vengpack");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("lod_unmapped.mesh.json") != string::npos);
    CHECK(reader.error().find("'Elsewhere'") != string::npos);
    CHECK(reader.error().find("map it with 'submeshes'") != string::npos);
}

TEST_CASE("Cooker: levels of detail on a skinned mesh are refused")
{
    const Result<ArchiveReader> reader =
        CookLodPack("lod_skinned_pack.json", "veng_cooker_lod_skinned.vengpack");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("'lod' is not supported on a skinned mesh") != string::npos);
}
