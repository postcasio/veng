// Cook timing test: the per-asset / per-importer instrument (Veng/Cook/CookTiming.h) a cook fills
// in when a report is asked for. What the report is read for is *attribution*, so the properties
// pinned here are the ones a wrong answer would come from: one record per manifest entry carrying
// that entry's own id and type, a cache hit distinguished from a fresh cook, and an accounting that
// adds up — the per-asset sum plus the named phases never exceeds the measured total, so the
// remainder a reader acts on is real rather than an artifact of double-counted time.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include "support/TempPath.h"

#include <fmt/format.h>

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/CookTiming.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // A fresh, process-unique scratch subdirectory.
    path UniqueDir(const char* label)
    {
        std::random_device rng;
        const path dir =
            TestSupport::TempDir() / fmt::format("veng_timing_{}_{:08x}", label, rng());
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A two-entry Raw pack, the cheapest manifest that still exercises per-entry attribution.
    path WriteRawPack(const path& dir)
    {
        std::filesystem::create_directories(dir / "data");
        for (const char* name : {"a.bin", "b.bin"})
        {
            std::ofstream out(dir / "data" / name, std::ios::binary);
            out << "PAYLOAD-" << name;
        }

        const path packJson = dir / "pack.json";
        std::ofstream out(packJson);
        out << R"({ "version": 1, "assets": [ )"
               R"({ "id": "0x00000000000003E9", "type": "Raw", "source": "data/a.bin" }, )"
               R"({ "id": "0x00000000000003EA", "type": "Raw", "source": "data/b.bin" } ] })";
        return packJson;
    }
}

TEST_CASE("CookTiming: a fresh cook records one attributed entry per manifest asset")
{
    const path packDir = UniqueDir("fresh");
    const path packJson = WriteRawPack(packDir);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    CookTiming timing;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "out.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, nullptr, &timing)
                .has_value());

    REQUIRE(timing.Assets.size() == 2);
    CHECK(timing.Assets[0].Id.Value == 0x00000000000003E9ULL);
    CHECK(timing.Assets[1].Id.Value == 0x00000000000003EAULL);
    for (const CookAssetTiming& asset : timing.Assets)
    {
        CHECK(asset.Type == "Raw");
        CHECK_FALSE(asset.CacheHit);
        CHECK(asset.TotalSeconds() >= 0.0);
    }
    CHECK(timing.CacheHits() == 0);

    // The archive write is measured even when nothing else is; a zero here would mean the phase
    // was never sampled and its share of the remainder is silently unattributed.
    CHECK(timing.ArchiveWriteSeconds > 0.0);
}

TEST_CASE("CookTiming: a cache hit is recorded as a hit and costs no importer time")
{
    const path packDir = UniqueDir("hit");
    const path packJson = WriteRawPack(packDir);

    Result<CookCache> cache = CookCache::Open(UniqueDir("hitcache"), "test-tool-tag");
    REQUIRE(cache.has_value());

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    CookTiming cold;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "cold.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, &*cache, &cold)
                .has_value());
    CHECK(cold.CacheHits() == 0);

    CookTiming warm;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "warm.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, &*cache, &warm)
                .has_value());

    REQUIRE(warm.Assets.size() == 2);
    CHECK(warm.CacheHits() == 2);
    for (const CookAssetTiming& asset : warm.Assets)
    {
        CHECK(asset.CacheHit);
        CHECK(asset.ImportSeconds == 0.0);
        CHECK(asset.StoreSeconds == 0.0);
    }
}

TEST_CASE("CookTiming: the per-asset sum and the named phases reconcile to the total")
{
    const path packDir = UniqueDir("account");
    const path packJson = WriteRawPack(packDir);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // Cooked with one job: the reconciliation is a property of a sequential cook, since assets that
    // ran concurrently sum to more wall time than the invocation contains.
    const CookStopwatch invocation;
    CookTiming timing;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "out.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, nullptr, &timing, 1)
                .has_value());
    timing.TotalSeconds = invocation.Elapsed();

    // Every measured phase is disjoint, so their sum cannot exceed the invocation they happened
    // inside — the property that makes the reported remainder a real quantity rather than a
    // subtraction of double-counted time.
    CHECK(timing.AssetSeconds() + timing.NamedPhaseSeconds() <= timing.TotalSeconds);

    // The report renders and names both halves of the accounting.
    const string summary = FormatCookTimingSummary(timing);
    CHECK(summary.find("assets") != string::npos);
    CHECK(summary.find("remainder") != string::npos);
    CHECK(summary.find("per importer") != string::npos);
}

TEST_CASE("CookTiming: a parallel cook reports a worker sum, not a wall-clock remainder")
{
    const path packDir = UniqueDir("parallelaccount");
    const path packJson = WriteRawPack(packDir);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const CookStopwatch invocation;
    CookTiming timing;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "out.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, nullptr, &timing, 4)
                .has_value());
    timing.TotalSeconds = invocation.Elapsed();

    // Above one job the per-asset seconds overlap each other, so `total - assets` is not a
    // remainder and is not offered as one; the report states the budget and labels the sum instead.
    CHECK(timing.Jobs == 4);
    const string summary = FormatCookTimingSummary(timing);
    CHECK(summary.find("4 jobs") != string::npos);
    CHECK(summary.find("summed across workers") != string::npos);
    CHECK(summary.find("remainder") == string::npos);
}

TEST_CASE("CookTiming: the CSV table carries a header and one row per asset")
{
    const path packDir = UniqueDir("csv");
    const path packJson = WriteRawPack(packDir);

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    CookTiming timing;
    REQUIRE(cooker
                .CookPack(packJson, packDir / "out.vengpack", {}, nullptr, nullptr, nullptr,
                          nullptr, {}, {}, nullptr, &timing)
                .has_value());

    const path csv = packDir / "timing.csv";
    REQUIRE(WriteCookTimingTable(csv, timing).has_value());

    std::ifstream in(csv);
    REQUIRE(in.is_open());
    usize lines = 0;
    string line;
    string header;
    while (std::getline(in, line))
    {
        if (lines == 0)
        {
            header = line;
        }
        ++lines;
    }
    CHECK(header == "id,type,cache_hit,cache_lookup_s,serialized_wait_s,import_s,store_s,total_s");
    CHECK(lines == timing.Assets.size() + 1);
}
