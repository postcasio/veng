// Cook cache test: the cooked-blob cache (Veng/Cook/CookCache.h) lets an unchanged asset's final
// compressed bytes be copied into the pack instead of re-cooked. Exercises the load-bearing
// properties: a cache hit is byte-identical to a fresh cook (including a multi-blob material entry
// and its resolved dependencies); the depfile stays complete across a hit; a source edit is not a
// false hit; a touch (mtime moved, content unchanged) still hits via the content-hash fallback; and
// two build configurations sharing one cache dir never contaminate each other.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include "support/TempPath.h"

#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Project/BuildConfiguration.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    vector<u8> ReadBytes(const path& file)
    {
        std::ifstream in(file, std::ios::binary);
        REQUIRE(in.is_open());
        return vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A fresh, process-unique scratch subdirectory (a cache root or a temp pack tree).
    path UniqueDir(const char* label)
    {
        std::random_device rng;
        const path dir = TestSupport::TempDir() / fmt::format("veng_cache_{}_{:08x}", label, rng());
        std::filesystem::create_directories(dir);
        return dir;
    }

    CookCache OpenCache()
    {
        Result<CookCache> cache = CookCache::Open(UniqueDir("cache"), "test-tool-tag");
        REQUIRE(cache.has_value());
        return std::move(*cache);
    }
}

TEST_CASE("CookCache: a cache hit is byte-identical to a fresh cook, across a multi-blob entry")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "material_pack.json";
    const path shaderInclude = path(VENG_CORE_SHADER_DIR);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    const path coldArchive = UniqueDir("cold") / "out.vengpack";
    const path warmArchive = UniqueDir("warm") / "out.vengpack";

    // Cold cook populates the cache; warm cook must serve every entry from it.
    const VoidResult cold = cooker.CookPack(packJson, coldArchive, {}, nullptr, nullptr, nullptr,
                                            nullptr, {}, shaderInclude, &cache);
    REQUIRE(cold.has_value());
    const VoidResult warm = cooker.CookPack(packJson, warmArchive, {}, nullptr, nullptr, nullptr,
                                            nullptr, {}, shaderInclude, &cache);
    REQUIRE(warm.has_value());

    // Byte-identical archives prove the replayed stored blobs match a fresh compress.
    CHECK(ReadBytes(coldArchive) == ReadBytes(warmArchive));

    // The material entry emits two blobs (the Material and its default MaterialInstance); both must
    // survive the cache round-trip and resolve back out of the warm archive.
    const Result<ArchiveReader> reader = ArchiveReader::Open(warmArchive);
    REQUIRE(reader.has_value());
    CHECK(reader->Find(AssetId{0x0000000000000BB9}).has_value()); // the parent Material
    CHECK(reader->Find(AssetId{0x000000000089582B}).has_value()); // its default MaterialInstance
}

TEST_CASE("CookCache: the depfile stays complete when every entry is a cache hit")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "material_pack.json";
    const path shaderInclude = path(VENG_CORE_SHADER_DIR);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    const path a = UniqueDir("depa") / "out.vengpack";
    const path b = UniqueDir("depb") / "out.vengpack";

    vector<path> coldDeps;
    vector<path> warmDeps;
    REQUIRE(cooker
                .CookPack(packJson, a, {}, nullptr, nullptr, &coldDeps, nullptr, {}, shaderInclude,
                          &cache)
                .has_value());
    REQUIRE(cooker
                .CookPack(packJson, b, {}, nullptr, nullptr, &warmDeps, nullptr, {}, shaderInclude,
                          &cache)
                .has_value());

    // A cache hit skips the importer, so the depfile must be reconstructed from the cached
    // dependency list — the two runs record exactly the same sorted, de-duplicated sources.
    CHECK_FALSE(coldDeps.empty());
    CHECK(coldDeps == warmDeps);
}

TEST_CASE("CookCache: editing a source is not a false cache hit")
{
    const path packDir = UniqueDir("editpack");
    std::filesystem::create_directories(packDir / "data");

    const path source = packDir / "data" / "x.bin";
    {
        std::ofstream out(source, std::ios::binary);
        out << "ORIGINAL-CONTENT";
    }
    const path packJson = packDir / "pack.json";
    {
        std::ofstream out(packJson);
        out << R"({ "version": 1, "assets": [ { "id": "0x00000000000003E9", "type": "Raw", )"
               R"("source": "data/x.bin" } ] })";
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    const path first = UniqueDir("edit1") / "out.vengpack";
    const path second = UniqueDir("edit2") / "out.vengpack";
    const path third = UniqueDir("edit3") / "out.vengpack";

    REQUIRE(cooker.CookPack(packJson, first, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());

    // Change the source; the same relative name now hashes differently, so the cook must not reuse
    // the stale blob.
    {
        std::ofstream out(source, std::ios::binary);
        out << "DIFFERENT-CONTENT-!";
    }
    REQUIRE(
        cooker.CookPack(packJson, second, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
            .has_value());
    CHECK(ReadBytes(first) != ReadBytes(second));

    // Cooking again with no further change is a legitimate hit, identical to the second cook.
    REQUIRE(cooker.CookPack(packJson, third, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());
    CHECK(ReadBytes(second) == ReadBytes(third));
}

TEST_CASE("CookCache: touching a source (mtime change, same content) is still a hit")
{
    const path packDir = UniqueDir("touchpack");
    std::filesystem::create_directories(packDir / "data");

    const path source = packDir / "data" / "x.bin";
    {
        std::ofstream out(source, std::ios::binary);
        out << "STABLE-CONTENT-STAYS";
    }
    const path packJson = packDir / "pack.json";
    {
        std::ofstream out(packJson);
        out << R"({ "version": 1, "assets": [ { "id": "0x00000000000003E9", "type": "Raw", )"
               R"("source": "data/x.bin" } ] })";
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    const path first = UniqueDir("touch1") / "out.vengpack";
    const path second = UniqueDir("touch2") / "out.vengpack";
    REQUIRE(cooker.CookPack(packJson, first, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());

    // Move the mtime forward without changing the bytes: the size+mtime fast path misses, but the
    // content-hash fallback confirms the file is unchanged, so the cook is still a hit.
    const auto later = std::filesystem::last_write_time(source) + std::chrono::seconds(120);
    std::filesystem::last_write_time(source, later);

    REQUIRE(
        cooker.CookPack(packJson, second, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
            .has_value());
    CHECK(ReadBytes(first) == ReadBytes(second));
}

TEST_CASE("CookCache: an unchanged pack is not rewritten")
{
    const path packDir = UniqueDir("skippack");
    std::filesystem::create_directories(packDir / "data");

    const path source = packDir / "data" / "x.bin";
    {
        std::ofstream out(source, std::ios::binary);
        out << "PACK-CONTENT-FOR-SKIP";
    }
    const path packJson = packDir / "pack.json";
    {
        std::ofstream out(packJson);
        out << R"({ "version": 1, "assets": [ { "id": "0x00000000000003E9", "type": "Raw", )"
               R"("source": "data/x.bin" } ] })";
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    // Cook into a single stable output path so the second cook can see the first's file.
    const path out = UniqueDir("skipout") / "out.vengpack";
    REQUIRE(cooker.CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());
    const auto firstWrite = std::filesystem::last_write_time(out);

    // Re-cook with nothing changed: every entry hits and the existing pack already matches, so the
    // pack must not be rewritten — its mtime is preserved exactly.
    REQUIRE(cooker.CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());
    CHECK(std::filesystem::last_write_time(out) == firstWrite);

    // Change the source: the pack content differs, so it must be rewritten (a new mtime).
    {
        std::ofstream out2(source, std::ios::binary);
        out2 << "PACK-CONTENT-CHANGED!!";
    }
    REQUIRE(cooker.CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());
    CHECK(std::filesystem::last_write_time(out) != firstWrite);
}

TEST_CASE("CookCache: two build configurations share a cache dir without contaminating each other")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_bc7_pack.json";

    // A Windows (BC7) configuration and the zero-config ASTC default resolve the same texture to
    // different codecs — the two must key separately in one shared cache.
    const Result<BuildConfiguration> windows =
        ParseBuildConfiguration(fixtureDir / "windows.buildcfg");
    REQUIRE(windows.has_value());

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    const path bc7A = UniqueDir("bc7a") / "out.vengpack";
    const path astc = UniqueDir("astc") / "out.vengpack";
    const path bc7B = UniqueDir("bc7b") / "out.vengpack";

    REQUIRE(cooker
                .CookPack(packJson, bc7A, {}, nullptr, nullptr, nullptr, &*windows,
                          fixtureDir / "windows.buildcfg", {}, &cache)
                .has_value());
    REQUIRE(cooker.CookPack(packJson, astc, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache)
                .has_value());

    // The shared cache did not hand the ASTC cook the BC7 blob (or vice versa): distinct codecs,
    // distinct bytes.
    CHECK(ReadBytes(bc7A) != ReadBytes(astc));

    // Re-cooking the BC7 configuration serves its own cached blob, not the ASTC one that also lives
    // in the shared dir.
    REQUIRE(cooker
                .CookPack(packJson, bc7B, {}, nullptr, nullptr, nullptr, &*windows,
                          fixtureDir / "windows.buildcfg", {}, &cache)
                .has_value());
    CHECK(ReadBytes(bc7A) == ReadBytes(bc7B));
}
