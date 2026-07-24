// Collision-shape cook test: cooks a fixture collision pack through libveng_cook and checks the
// resulting CookedCollisionShapeHeader and geometry against the fixture cube.obj — the convex mode
// reducing the model's 24 split vertices to the cube's 8 corners, and the mesh mode welding them
// to the same 8 while keeping all 12 triangles.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // Reads a cooked collision blob's header and its point/index payload.
    struct DecodedShape
    {
        CookedCollisionShapeHeader Header{};
        vector<vec3> Points;
        vector<u32> Indices;
    };

    DecodedShape Decode(const std::span<const u8> blob)
    {
        DecodedShape decoded;
        std::memcpy(&decoded.Header, blob.data(), sizeof(decoded.Header));

        usize cursor = sizeof(decoded.Header);
        decoded.Points.resize(decoded.Header.PointCount);
        for (u32 i = 0; i < decoded.Header.PointCount; ++i)
        {
            f32 xyz[3] = {};
            std::memcpy(xyz, blob.data() + cursor, sizeof(xyz));
            cursor += sizeof(xyz);
            decoded.Points[i] = vec3(xyz[0], xyz[1], xyz[2]);
        }
        decoded.Indices.resize(decoded.Header.IndexCount);
        if (decoded.Header.IndexCount > 0)
        {
            std::memcpy(decoded.Indices.data(), blob.data() + cursor,
                        decoded.Indices.size() * sizeof(u32));
        }
        return decoded;
    }
}

TEST_CASE("Cooker: a convex collision shape reduces a model to its hull vertices")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "collision_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_collision.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x2C01});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::CollisionShape);
    REQUIRE(entry->Blob.size() >= sizeof(CookedCollisionShapeHeader));

    const DecodedShape shape = Decode(entry->Blob);
    CHECK(shape.Header.Version == CookedCollisionShapeVersion);
    CHECK(shape.Header.Mode == static_cast<u32>(CookedCollisionGeometry::Convex));

    // The source carries 24 vertices (4 per face, split for per-face normals and UVs); the cube's
    // hull has exactly 8 corners, and a convex blob carries no indices.
    CHECK(shape.Header.PointCount == 8);
    CHECK(shape.Header.IndexCount == 0);
    CHECK(entry->Blob.size() == sizeof(CookedCollisionShapeHeader) + 8 * 3 * sizeof(f32));

    // Every hull point is a corner of the unit cube, and all eight are distinct.
    for (const vec3 point : shape.Points)
    {
        CHECK(std::abs(point.x) == doctest::Approx(0.5f));
        CHECK(std::abs(point.y) == doctest::Approx(0.5f));
        CHECK(std::abs(point.z) == doctest::Approx(0.5f));
    }
    for (usize i = 0; i < shape.Points.size(); ++i)
    {
        for (usize j = i + 1; j < shape.Points.size(); ++j)
        {
            CHECK(shape.Points[i] != shape.Points[j]);
        }
    }
}

TEST_CASE("Cooker: a triangle-mesh collision shape welds the model's split vertices")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "collision_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_collision_mesh.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x2C02});
    REQUIRE(entry.has_value());

    const DecodedShape shape = Decode(entry->Blob);
    CHECK(shape.Header.Mode == static_cast<u32>(CookedCollisionGeometry::Mesh));

    // A solver wants the surface's real connectivity, not the render mesh's material/UV splits:
    // the 24 exported vertices weld to the cube's 8 corners and all 12 triangles survive.
    CHECK(shape.Header.PointCount == 8);
    CHECK(shape.Header.IndexCount == 36);
    for (const u32 index : shape.Indices)
    {
        CHECK(index < shape.Header.PointCount);
    }
}

TEST_CASE("Cooker: a warm collision-shape cook at 8 jobs replays the cache byte for byte")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "collision_pack.json";

    std::random_device rng;
    const path scratch = Veng::TestSupport::TempDir() / fmt::format("veng_collision_{:08x}", rng());
    std::filesystem::create_directories(scratch / "cache");
    std::filesystem::create_directories(scratch / "cold");
    std::filesystem::create_directories(scratch / "warm");

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    Result<CookCache> cache = CookCache::Open(scratch / "cache", "collision-shape-test-tag");
    REQUIRE(cache.has_value());

    const path coldArchive = scratch / "cold" / "out.vengpack";
    const path warmArchive = scratch / "warm" / "out.vengpack";

    // Cooked at 8 jobs, the width the warm-cook no-op property is guarded at for every other
    // importer: a new asset type must not break it.
    REQUIRE(cooker
                .CookPack(packJson, coldArchive, {}, nullptr, nullptr, nullptr, nullptr, {}, {},
                          &*cache, nullptr, 8)
                .has_value());
    REQUIRE(cooker
                .CookPack(packJson, warmArchive, {}, nullptr, nullptr, nullptr, nullptr, {}, {},
                          &*cache, nullptr, 8)
                .has_value());

    const auto readBytes = [](const path& file)
    {
        std::ifstream in(file, std::ios::binary);
        REQUIRE(in.is_open());
        return vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    CHECK(readBytes(coldArchive) == readBytes(warmArchive));
}
