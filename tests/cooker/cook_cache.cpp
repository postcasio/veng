// Cook cache test: the cooked-blob cache (Veng/Cook/CookCache.h) lets an unchanged asset's final
// compressed bytes be copied into the pack instead of re-cooked. Exercises the load-bearing
// properties: a cache hit is byte-identical to a fresh cook (including a multi-blob material entry
// and its resolved dependencies); the depfile stays complete across a hit; a source edit is not a
// false hit; a touch (mtime moved, content unchanged) still hits via the content-hash fallback; and
// two build configurations sharing one cache dir never contaminate each other. It also pins the
// tool tag: a blob's bytes come from code as well as data, so rebuilding an image the cook runs an
// importer from must invalidate what that image produced.

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
#include <Veng/Cook/CookModule.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Project/BuildConfiguration.h>

#include "module/probe_component.h"

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

    // The asset the probe pack declares; the importer cooks its "value" into four bytes.
    constexpr AssetId ProbeAsset{0x0000000000002041};

    // Cooks the probe pack through the importer in @p cookModulePath, against a cache in @p cacheDir
    // opened exactly as vengc opens it — with the tool tag that module identity produces. Returns
    // the blob the resulting archive carries, which is what the two images must disagree on.
    vector<u8> CookProbeThrough(const path& cookModulePath, const path& cacheDir,
                                const path& packJson, const path& out)
    {
        // Both module handles outlive the Cooker the importers move into: their code lives in the
        // dlopened images.
        Result<LoadedModuleTypes> moduleTypes = LoadModuleTypes(path{VENG_TEST_MODULE_PATH});
        REQUIRE(moduleTypes.has_value());
        Result<LoadedCookModule> cookModule = LoadCookModule(cookModulePath);
        REQUIRE(cookModule.has_value());

        Result<CookCache> cache = CookCache::Open(
            cacheDir, ComputeCookToolTag({}, path{VENG_TEST_MODULE_PATH}, cookModulePath));
        REQUIRE(cache.has_value());

        Cooker cooker;
        MergeAssetTypes(moduleTypes->AssetTypes, cooker.GetAssetTypes());
        cookModule->Importers.MoveInto(cooker);

        REQUIRE(
            cooker.CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &*cache)
                .has_value());

        const Result<ArchiveReader> reader = ArchiveReader::Open(out);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(ProbeAsset);
        REQUIRE(entry.has_value());
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
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

TEST_CASE("CookCache: a parallel cook's warm re-cook rewrites nothing")
{
    // The property a parallel driver most easily breaks, and it is a cache-correctness one. The
    // cook cache concludes a pack on disk is already what would be written by laying out the archive
    // TOC from cached descriptors and hashing it — so a TOC ordered by completion rather than by the
    // manifest would make every warm cook rewrite packs it did not need to.
    const path packDir = UniqueDir("parallelskip");
    std::filesystem::create_directories(packDir / "data");

    // Enough entries that the pool genuinely runs several at once, and distinct content so a
    // mis-ordered TOC cannot coincidentally hash the same.
    constexpr usize AssetCount = 24;
    string entries;
    for (usize i = 0; i < AssetCount; ++i)
    {
        const path source = packDir / "data" / fmt::format("x{}.bin", i);
        {
            std::ofstream out(source, std::ios::binary);
            out << fmt::format("PARALLEL-PACK-CONTENT-{:04}", i);
        }
        entries +=
            fmt::format(R"({}{{ "id": "0x{:016X}", "type": "Raw", "source": "data/x{}.bin" }})",
                        i == 0 ? "" : ", ", 0x5000 + i, i);
    }
    const path packJson = packDir / "pack.json";
    {
        std::ofstream out(packJson);
        out << fmt::format(R"({{ "version": 1, "assets": [ {} ] }})", entries);
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const CookCache cache = OpenCache();

    constexpr u32 Jobs = 8;
    const path out = UniqueDir("parallelskipout") / "out.vengpack";
    REQUIRE(cooker
                .CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache,
                          nullptr, Jobs)
                .has_value());
    const auto firstWrite = std::filesystem::last_write_time(out);

    // Re-cook with nothing changed, again across the pool: every entry hits, the TOC hashes to the
    // same digest whatever order the workers ran in, and the pack is left untouched.
    REQUIRE(cooker
                .CookPack(packJson, out, {}, nullptr, nullptr, nullptr, nullptr, {}, {}, &cache,
                          nullptr, Jobs)
                .has_value());
    CHECK(std::filesystem::last_write_time(out) == firstWrite);
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

TEST_CASE("CookCache: a rebuilt cook module's importer is not replayed from the previous build")
{
    const path packDir = UniqueDir("modkeypack");
    const path packJson = packDir / "pack.json";
    {
        std::ofstream out(packJson);
        out << R"({ "version": 1, "assets": [ { "id": "0x0000000000002041", "type": ")"
            << ProbeAssetTypeName << R"(", "value": 305419896 } ] })";
    }

    // One cache dir across both cooks — the warm cache is the whole point.
    const path cacheDir = UniqueDir("modkeycache");

    const vector<u8> first = CookProbeThrough(path{VENG_TEST_COOK_MODULE_PATH}, cacheDir, packJson,
                                              UniqueDir("modkey1") / "out.vengpack");
    CHECK(first == vector<u8>{0x78, 0x56, 0x34, 0x12});

    // The same unchanged source, cooked against the warm cache by a different image of the cook
    // module whose importer emits the value in the opposite byte order. Nothing the old key covered
    // moved, so a cache blind to the image that produced a blob replays the first build's bytes.
    const vector<u8> second = CookProbeThrough(path{VENG_TEST_COOK_MODULE_REVERSED_PATH}, cacheDir,
                                               packJson, UniqueDir("modkey2") / "out.vengpack");
    CHECK(second == vector<u8>{0x12, 0x34, 0x56, 0x78});
}

TEST_CASE("CookCache: the tool tag follows a module image's contents, not only its path")
{
    const path dir = UniqueDir("tooltag");
    const path image = dir / "module.bin";
    {
        std::ofstream out(image, std::ios::binary);
        out << "FIRST-BUILD";
    }
    const string before = ComputeCookToolTag({}, {}, image);

    // A rebuild lands new bytes at the same path — what an incremental build does every time, and
    // the case a path-only fingerprint cannot see.
    {
        std::ofstream out(image, std::ios::binary | std::ios::trunc);
        out << "SECOND-BUILD-IS-LONGER";
    }
    CHECK(before != ComputeCookToolTag({}, {}, image));

    // The two module slots key separately, so a runtime-module change is never mistaken for a
    // cook-module change (or vice versa) by an accidental fingerprint collision.
    CHECK(ComputeCookToolTag({}, image, {}) != ComputeCookToolTag({}, {}, image));

    // A cook that loads no module at all keys as it always did — the format version alone.
    CHECK(ComputeCookToolTag({}, {}, {}) == ComputeCookToolTag({}, {}, {}));
    CHECK(ComputeCookToolTag({}, {}, {}) != ComputeCookToolTag({}, {}, image));
}
