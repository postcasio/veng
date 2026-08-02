// The persistent derived-data cache and the texture blob that rides in it, against a temp
// directory and no device.
//
// The properties: a payload round-trips across a close and a re-open; a differing generation wipes
// the cache rather than reading stale entries; the caps evict oldest-touched over both count and
// bytes with the index and the directory agreeing afterwards; and every way a blob can be damaged —
// truncated, bit-flipped, deleted outright, or left behind as a half-written temp file — reads as a
// miss rather than as an error or as texels.
//
// GeneratedTextureBlob is renderer-internal (engine/src is on this target's include path) and is
// pure sizing arithmetic over a byte layout, so its round trip and its rejections are pinned here
// beside the cache that carries its payloads.

#include <atomic>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Persistence/DerivedDataCache.h>

#include <support/TempPath.h>

#include "Renderer/GeneratedTextureBlob.h"

using namespace Veng;

namespace
{
    // A fresh, unique cache directory per case under the process's scratch tree, removed on
    // destruction.
    struct TempRoot
    {
        path Dir;

        TempRoot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("ddc-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempRoot() { std::filesystem::remove_all(Dir); }
    };

    vector<u8> Bytes(const u8 seed, const usize length)
    {
        vector<u8> bytes(length);
        for (usize i = 0; i < length; i++)
        {
            bytes[i] = static_cast<u8>(seed + i);
        }
        return bytes;
    }

    Unique<DerivedDataCache> OpenCache(const path& root, const string& generation,
                                       const u64 maxEntries = 1024,
                                       const u64 maxBytes = 1024 * 1024)
    {
        Result<Unique<DerivedDataCache>> cache = DerivedDataCache::Open({
            .Root = root,
            .Generation = generation,
            .MaxEntries = maxEntries,
            .MaxBytes = maxBytes,
        });
        REQUIRE(cache.has_value());
        return std::move(*cache);
    }

    // The blob files the cache currently holds on disk, index or no index.
    vector<path> BlobFiles(const path& root)
    {
        vector<path> files;
        for (const std::filesystem::directory_entry& file :
             std::filesystem::directory_iterator(root))
        {
            if (file.path().extension() == ".blob")
            {
                files.push_back(file.path());
            }
        }
        return files;
    }
}

TEST_CASE("derived data cache: a payload survives a close and a re-open")
{
    const TempRoot root;
    const vector<u8> payload = Bytes(7, 512);

    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen-a");
        CHECK(cache->Store("planet/13", payload));
        CHECK_FALSE(cache->Read("planet/14").has_value());
    }

    const Unique<DerivedDataCache> reopened = OpenCache(root.Dir, "gen-a");
    CHECK(reopened->Contains("planet/13"));
    const optional<vector<u8>> read = reopened->Read("planet/13");
    REQUIRE(read.has_value());
    CHECK(*read == payload);

    const DerivedDataCacheStats stats = reopened->GetStats();
    CHECK(stats.Entries == 1u);
    CHECK(stats.Bytes == payload.size());
    CHECK(stats.Hits == 1u);
    CHECK(stats.Misses == 0u);
}

TEST_CASE("derived data cache: a differing generation wipes the cache")
{
    const TempRoot root;

    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen-a");
        CHECK(cache->Store("a", Bytes(1, 64)));
        CHECK(cache->Store("b", Bytes(2, 64)));
        CHECK(BlobFiles(root.Dir).size() == 2u);
    }

    const Unique<DerivedDataCache> next = OpenCache(root.Dir, "gen-b");
    CHECK(next->GetStats().Entries == 0u);
    CHECK(next->GetStats().Bytes == 0u);
    CHECK_FALSE(next->Read("a").has_value());
    CHECK(BlobFiles(root.Dir).empty());

    // And the wipe is not a one-way door: the same generation stores and reads again.
    CHECK(next->Store("a", Bytes(3, 64)));
    CHECK(next->Read("a").has_value());
}

TEST_CASE("derived data cache: the entry cap evicts the oldest-touched")
{
    const TempRoot root;
    const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen", 3, 1024 * 1024);

    CHECK(cache->Store("a", Bytes(1, 32)));
    CHECK(cache->Store("b", Bytes(2, 32)));
    CHECK(cache->Store("c", Bytes(3, 32)));
    // Reading "a" touches it, so it now outranks "b" and "c" for eviction.
    CHECK(cache->Read("a").has_value());

    CHECK(cache->Store("d", Bytes(4, 32)));
    CHECK(cache->Store("e", Bytes(5, 32)));

    CHECK(cache->Contains("a"));
    CHECK_FALSE(cache->Contains("b"));
    CHECK_FALSE(cache->Contains("c"));
    CHECK(cache->Contains("d"));
    CHECK(cache->Contains("e"));

    const DerivedDataCacheStats stats = cache->GetStats();
    CHECK(stats.Entries == 3u);
    CHECK(stats.Evictions == 2u);
    // The index and the directory agree: an evicted entry's file is gone, not merely unindexed.
    CHECK(BlobFiles(root.Dir).size() == stats.Entries);
}

TEST_CASE("derived data cache: the byte cap evicts until the total fits")
{
    const TempRoot root;
    const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen", 1024, 300);

    for (const string& key : {string("a"), string("b"), string("c"), string("d")})
    {
        CHECK(cache->Store(key, Bytes(1, 100)));
    }

    const DerivedDataCacheStats stats = cache->GetStats();
    CHECK(stats.Bytes <= 300u);
    CHECK(stats.Entries == 3u);
    CHECK_FALSE(cache->Contains("a"));
    CHECK(cache->Contains("d"));
    CHECK(BlobFiles(root.Dir).size() == stats.Entries);
}

TEST_CASE("derived data cache: a damaged blob reads as a miss and is removed")
{
    const TempRoot root;

    SUBCASE("truncated")
    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen");
        REQUIRE(cache->Store("k", Bytes(9, 256)));
        const vector<path> files = BlobFiles(root.Dir);
        REQUIRE(files.size() == 1u);
        std::filesystem::resize_file(files[0], 12);

        CHECK_FALSE(cache->Read("k").has_value());
        CHECK_FALSE(cache->Contains("k"));
        CHECK(BlobFiles(root.Dir).empty());
        CHECK(cache->GetStats().Entries == 0u);
    }

    SUBCASE("bit-flipped")
    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen");
        REQUIRE(cache->Store("k", Bytes(9, 256)));
        const vector<path> files = BlobFiles(root.Dir);
        REQUIRE(files.size() == 1u);
        {
            std::fstream stream(files[0], std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(stream.good());
            stream.seekp(30, std::ios::beg);
            const char flipped = 0x5A;
            stream.write(&flipped, 1);
        }

        CHECK_FALSE(cache->Read("k").has_value());
        CHECK_FALSE(cache->Contains("k"));
        CHECK(BlobFiles(root.Dir).empty());
    }

    SUBCASE("deleted outright")
    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen");
        REQUIRE(cache->Store("k", Bytes(9, 256)));
        const vector<path> files = BlobFiles(root.Dir);
        REQUIRE(files.size() == 1u);
        std::filesystem::remove(files[0]);

        CHECK_FALSE(cache->Read("k").has_value());
        CHECK_FALSE(cache->Contains("k"));
    }
}

TEST_CASE("derived data cache: an interrupted write leaves nothing behind")
{
    const TempRoot root;

    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen");
        REQUIRE(cache->Store("k", Bytes(4, 128)));
    }

    // What a crash mid-store leaves: a temp file the rename never claimed, and a blob file no
    // index names.
    {
        std::ofstream stray(root.Dir / "cache.00000000000000ff.blob.tmp", std::ios::binary);
        stray << "half a blob";
    }
    {
        std::ofstream orphan(root.Dir / "cache.00000000000000fe.blob", std::ios::binary);
        orphan << "an unindexed blob";
    }

    const Unique<DerivedDataCache> reopened = OpenCache(root.Dir, "gen");
    CHECK(reopened->Contains("k"));
    CHECK(reopened->Read("k").has_value());
    CHECK_FALSE(std::filesystem::exists(root.Dir / "cache.00000000000000ff.blob.tmp"));
    CHECK_FALSE(std::filesystem::exists(root.Dir / "cache.00000000000000fe.blob"));
    CHECK(BlobFiles(root.Dir).size() == 1u);
}

TEST_CASE("derived data cache: a file the cache did not write is left alone")
{
    const TempRoot root;
    std::filesystem::create_directories(root.Dir);
    {
        std::ofstream foreign(root.Dir / "notes.txt");
        foreign << "not the cache's";
    }

    {
        const Unique<DerivedDataCache> cache = OpenCache(root.Dir, "gen-a");
        REQUIRE(cache->Store("k", Bytes(1, 32)));
    }
    // The generation change wipes every entry, and only entries.
    const Unique<DerivedDataCache> next = OpenCache(root.Dir, "gen-b");
    CHECK(next->GetStats().Entries == 0u);
    CHECK(std::filesystem::exists(root.Dir / "notes.txt"));
}

TEST_CASE("generated texture blob: shapes and texels round-trip")
{
    using namespace Veng::Renderer;

    GeneratedTextureBlob blob;
    blob.Shapes.push_back({.TexelFormat = Format::RGBA8Unorm,
                           .Type = ImageType::Type2D,
                           .Extent = {4, 4, 1},
                           .Layers = 1,
                           .MipLevels = 3});
    blob.Shapes.push_back({.TexelFormat = Format::RG16Sfloat,
                           .Type = ImageType::Type2D,
                           .Extent = {2, 2, 1},
                           .Layers = 6,
                           .MipLevels = 1});

    // 4x4 + 2x2 + 1x1 at 4 bytes, then six 2x2 layers at 4 bytes.
    CHECK(GeneratedTextureShapeBytes(blob.Shapes[0]) == 84u);
    CHECK(GeneratedTextureShapeBytes(blob.Shapes[1]) == 96u);
    CHECK(GeneratedTextureMipOffset(blob.Shapes[0], 1) == 64u);
    CHECK(GeneratedTextureMipOffset(blob.Shapes[0], 2) == 80u);

    blob.Texels.resize(84 + 96);
    for (usize i = 0; i < blob.Texels.size(); i++)
    {
        blob.Texels[i] = static_cast<u8>(i * 7);
    }

    const vector<u8> payload = EncodeGeneratedTextureBlob(blob);
    REQUIRE_FALSE(payload.empty());

    const optional<GeneratedTextureBlob> decoded = DecodeGeneratedTextureBlob(payload);
    REQUIRE(decoded.has_value());
    CHECK(decoded->Shapes == blob.Shapes);
    CHECK(decoded->Texels == blob.Texels);
}

TEST_CASE("generated texture blob: a payload that is not one decodes to nothing")
{
    using namespace Veng::Renderer;

    GeneratedTextureBlob blob;
    blob.Shapes.push_back({.TexelFormat = Format::RGBA8Unorm,
                           .Type = ImageType::Type2D,
                           .Extent = {4, 4, 1},
                           .Layers = 1,
                           .MipLevels = 1});
    blob.Texels.resize(64);
    const vector<u8> payload = EncodeGeneratedTextureBlob(blob);
    REQUIRE_FALSE(payload.empty());

    CHECK_FALSE(DecodeGeneratedTextureBlob({}).has_value());
    CHECK_FALSE(
        DecodeGeneratedTextureBlob(std::span(payload).first(payload.size() - 1)).has_value());
    CHECK_FALSE(DecodeGeneratedTextureBlob(std::span(payload).first(6)).has_value());

    vector<u8> foreign = payload;
    foreign[0] = 'X';
    CHECK_FALSE(DecodeGeneratedTextureBlob(foreign).has_value());

    // Texels that do not add up to what the shapes describe are refused at the encode, so a short
    // readback set stores nothing rather than an entry that would decode into the wrong texels.
    GeneratedTextureBlob shortBlob = blob;
    shortBlob.Texels.resize(60);
    CHECK(EncodeGeneratedTextureBlob(shortBlob).empty());

    // A format with no known byte size cannot be sized, so it cannot be stored either.
    GeneratedTextureBlob unsized = blob;
    unsized.Shapes[0].TexelFormat = Format::Undefined;
    CHECK(EncodeGeneratedTextureBlob(unsized).empty());
}
