// assetpack round-trip tests: pure, no GPU. Proves the .vengpack
// container's writer/reader contract.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <set>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

using namespace Veng;

namespace
{
    vector<u8> Bytes(std::initializer_list<u8> values)
    {
        return vector<u8>(values.begin(), values.end());
    }
}

TEST_CASE("ArchiveWriter/Reader: byte-exact multi-entry round trip")
{
    ArchiveWriter writer;
    writer.Add(AssetId{0x3E9}, AssetType::Texture, Bytes({1, 2, 3, 4, 5}));
    writer.Add(AssetId{0x3EA}, AssetType::Mesh, Bytes({10, 20, 30}));
    writer.Add(AssetId{0x3EB}, AssetType::Material, Bytes({}));

    const vector<u8> archive = writer.Build();

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(archive);
    REQUIRE(reader.has_value());

    CHECK(reader->Entries().size() == 3);

    const optional<ArchiveEntry> texture = reader->Find(AssetId{0x3E9});
    REQUIRE(texture.has_value());
    CHECK(texture->Type == AssetType::Texture);
    CHECK(std::ranges::equal(texture->Blob, Bytes({1, 2, 3, 4, 5})));

    const optional<ArchiveEntry> mesh = reader->Find(AssetId{0x3EA});
    REQUIRE(mesh.has_value());
    CHECK(mesh->Type == AssetType::Mesh);
    CHECK(std::ranges::equal(mesh->Blob, Bytes({10, 20, 30})));

    const optional<ArchiveEntry> material = reader->Find(AssetId{0x3EB});
    REQUIRE(material.has_value());
    CHECK(material->Type == AssetType::Material);
    CHECK(material->Blob.empty());
}

TEST_CASE("ArchiveReader::Find: hits and misses")
{
    ArchiveWriter writer;
    writer.Add(AssetId{0x2A}, AssetType::Raw, Bytes({0xAB}));

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(writer.Build());
    REQUIRE(reader.has_value());

    CHECK(reader->Find(AssetId{0x2A}).has_value());
    CHECK_FALSE(reader->Find(AssetId{0x2B}).has_value());
    CHECK_FALSE(reader->Find(AssetId{0}).has_value());
}

TEST_CASE("ArchiveReader: rejects bad magic")
{
    // A full (32-byte) v2 header's worth of zeroes, so the magic check is the
    // first thing to fail rather than the buffer-too-small guard.
    vector<u8> bytes(32, 0);
    bytes[0] = 'N';
    bytes[1] = 'O';
    bytes[2] = 'P';
    bytes[3] = 'E';

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(bytes);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("magic") != string::npos);
}

TEST_CASE("ArchiveReader: rejects version mismatch")
{
    ArchiveWriter writer;
    writer.Add(AssetId{0x1}, AssetType::Raw, Bytes({1}));

    vector<u8> archive = writer.Build();

    // Header layout: magic[8], version (u32), count (u32) — corrupt the version.
    const u32 badVersion = ArchiveFormatVersion + 1;
    std::memcpy(archive.data() + 8, &badVersion, sizeof(badVersion));

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(archive);
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("version") != string::npos);
}

TEST_CASE("ArchiveReader: sorted TOC and Find over a large id set")
{
    std::mt19937_64 rng(0xA55E7F0);
    std::uniform_int_distribution<u64> dist(1, std::numeric_limits<u64>::max());

    std::set<u64> ids;
    while (ids.size() < 500)
        ids.insert(dist(rng));

    ArchiveWriter writer;
    for (const u64 id : ids)
        writer.Add(AssetId{id}, AssetType::Raw, Bytes({static_cast<u8>(id & 0xFF)}));

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(writer.Build());
    REQUIRE(reader.has_value());

    const vector<ArchiveTocEntry>& entries = reader->Entries();
    REQUIRE(entries.size() == ids.size());

    CHECK(std::ranges::is_sorted(entries, {}, [](const ArchiveTocEntry& e) { return e.Id.Value; }));

    for (const u64 id : ids)
    {
        const optional<ArchiveEntry> found = reader->Find(AssetId{id});
        REQUIRE(found.has_value());
        CHECK(found->Blob[0] == static_cast<u8>(id & 0xFF));
    }

    CHECK_FALSE(reader->Find(AssetId{0}).has_value());
}
