// The normal-variance companion: what the cook measures from a normal map's normals per mip level,
// and what it writes beside the map. The measure is checked on synthetic maps (a flat one carries
// no spread at any level; a noisy one's spread grows level by level as each averages more normals),
// and the cook on an uncompressed one whose stored bytes are readable.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/NormalVariance.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Types.h>

#include "Importers/NormalVariance.h"
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId FlatId{0x468134B04010819AULL};
    constexpr AssetId CheckerId{0xCB350F990ECBD4D6ULL};
    constexpr AssetId CheckerNoVarianceId{0x51143EF89C7B374BULL};

    // An RGBA8 normal map of `size`² texels, every texel the same encoded normal.
    vector<u8> UniformMap(u32 size, u8 x, u8 y)
    {
        vector<u8> rgba(static_cast<usize>(size) * size * 4);
        for (usize i = 0; i < rgba.size(); i += 4)
        {
            rgba[i] = x;
            rgba[i + 1] = y;
            rgba[i + 2] = 255;
            rgba[i + 3] = 255;
        }
        return rgba;
    }

    // The mean of a level's variances.
    f64 MeanOf(const vector<f32>& level)
    {
        f64 sum = 0.0;
        for (const f32 value : level)
        {
            sum += value;
        }
        return sum / static_cast<f64>(level.size());
    }

    // The written texture blob of `id` in a cook of `packJson` with no build configuration.
    vector<u8> CookBlob(const path& packJson, AssetId id)
    {
        std::random_device rng;
        const path archive = Veng::TestSupport::TempDir() /
                             fmt::format("veng_cooker_normal_variance_{:08x}.vengpack", rng());
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(packJson, archive).has_value());
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(id);
        REQUIRE(entry.has_value());
        const vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(archive);
        return blob;
    }
}

TEST_CASE("Normal variance: a flat or uniformly tilted map spreads nothing at any level")
{
    // Odd-free and odd extents both: 32² halves evenly, 24x24 clamps an odd edge at 3x3.
    for (const auto& [size, x, y] : {std::tuple<u32, u8, u8>{32, 128, 128}, {24, 200, 90}})
    {
        CAPTURE(size);
        const vector<u8> rgba = UniformMap(size, x, y);
        const vector<vector<f32>> chain =
            NormalVarianceChain(NormalMomentsOfRgba8(rgba, size, size), 6);
        REQUIRE(chain.size() == 6);
        f32 largest = 0.0f;
        for (const vector<f32>& level : chain)
        {
            largest = std::max(largest, *std::ranges::max_element(level));
        }
        CHECK(largest == 0.0f);
    }
}

TEST_CASE("Normal variance: a noisy map's spread rises with every level it averages")
{
    // Independent per-texel tilts of up to about 25 degrees: detail at every scale, so each level
    // averages more of it away than the one before.
    constexpr u32 Size = 64;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> tilt(128 - 50, 128 + 50);
    vector<u8> rgba(static_cast<usize>(Size) * Size * 4);
    for (usize i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = static_cast<u8>(tilt(rng));
        rgba[i + 1] = static_cast<u8>(tilt(rng));
        rgba[i + 2] = 255;
        rgba[i + 3] = 255;
    }

    const vector<vector<f32>> chain =
        NormalVarianceChain(NormalMomentsOfRgba8(rgba, Size, Size), 7);
    REQUIRE(chain.size() == 7);
    CHECK(chain.back().size() == 1);
    // The source level covers one normal per texel.
    CHECK(*std::ranges::max_element(chain[0]) == 0.0f);

    u32 rising = 0;
    f32 largest = 0.0f;
    for (usize level = 1; level < chain.size(); ++level)
    {
        rising += MeanOf(chain[level]) > MeanOf(chain[level - 1]) ? 1u : 0u;
        largest = std::max(largest, *std::ranges::max_element(chain[level]));
    }
    CHECK(rising == chain.size() - 1);
    CHECK(largest <= 1.0f);
    // A footprint of the whole map holds the population's spread, which for tilts this size is a
    // modest widening, far short of a fully rough lobe.
    CHECK(chain.back()[0] > 0.02f);
    CHECK(chain.back()[0] < 0.5f);
}

TEST_CASE("Normal variance: a downscaled base level keeps the spread its downscale averaged")
{
    // Alternating tilts one texel wide, resampled to half size: each base texel now covers both.
    constexpr u32 Size = 16;
    vector<u8> rgba(static_cast<usize>(Size) * Size * 4);
    for (u32 y = 0; y < Size; ++y)
    {
        for (u32 x = 0; x < Size; ++x)
        {
            u8* t = rgba.data() + ((static_cast<usize>(y) * Size + x) * 4);
            t[0] = ((x + y) % 2 == 0) ? 180 : 76;
            t[1] = 128;
            t[2] = 235;
            t[3] = 255;
        }
    }
    const NormalMoments resampled =
        ResampleNormalMoments(NormalMomentsOfRgba8(rgba, Size, Size), Size / 2, Size / 2);
    const vector<vector<f32>> chain = NormalVarianceChain(resampled, 1);
    CHECK(MeanOf(chain[0]) > 0.05);
}

TEST_CASE("Normal variance: the cook writes a Normal-role map's companion after its own chain")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path pack = fixtureDir / "texture_normal_variance_pack.json";

    // Uncompressed ("compression": "None"), so the companion is RGBA8 and its bytes read directly.
    const auto companion = [&](AssetId id) -> std::pair<CookedTextureHeader, vector<u8>>
    {
        const vector<u8> blob = CookBlob(pack, id);
        CookedTextureHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        usize chainBytes = 0;
        for (u32 level = 0; level < header.MipCount; ++level)
        {
            chainBytes += static_cast<usize>(std::max(1u, header.Width >> level)) *
                          std::max(1u, header.Height >> level) * 4;
        }
        const usize start = sizeof(header) + chainBytes;
        REQUIRE(blob.size() >= start);
        return {header, vector<u8>(blob.begin() + static_cast<std::ptrdiff_t>(start), blob.end())};
    };

    const auto [flatHeader, flat] = companion(FlatId);
    CHECK(flatHeader.VarianceFormat == static_cast<u32>(Renderer::Format::RGBA8Unorm));
    CHECK(flatHeader.MipCount == 5);
    // 16², 8², 4², 2², 1² RGBA8 texels: the companion matches the chain level for level.
    REQUIRE(flat.size() == (256 + 64 + 16 + 4 + 1) * 4);
    CHECK(std::ranges::all_of(flat, [&](u8 v) { return v == 0 || v == 255; }));
    u32 flatNonZero = 0;
    for (usize i = 0; i < flat.size(); i += 4)
    {
        flatNonZero += flat[i] != 0 ? 1u : 0u;
    }
    CHECK(flatNonZero == 0u);

    // One-texel alternating tilts: the source level is sharp, every coarser level averages two
    // opposite normals and stores the same spread, the fourth root of the alpha² it adds.
    const auto [checkerHeader, checker] = companion(CheckerId);
    CHECK(checkerHeader.VarianceFormat == static_cast<u32>(Renderer::Format::RGBA8Unorm));
    REQUIRE(checker.size() == flat.size());
    // Two normals at equal weight spread 1 − |mean|² about their mean.
    const auto decode = [](u8 encodedX)
    {
        const f32 x = (static_cast<f32>(encodedX) / 255.0f * 2.0f) - 1.0f;
        const f32 y = (128.0f / 255.0f * 2.0f) - 1.0f;
        return vec3(x, y, std::sqrt(1.0f - (x * x) - (y * y)));
    };
    const vec3 mean = 0.5f * (decode(180) + decode(76));
    const f32 spread = 1.0f - glm::dot(mean, mean);
    const auto expected = static_cast<i32>(
        std::lround(EncodeNormalVariance(NormalVarianceOfSpread(spread)) * 255.0f));
    i32 levelZeroMax = 0;
    i32 coarserMin = 255;
    i32 coarserMax = 0;
    for (usize i = 0; i < checker.size(); i += 4)
    {
        const i32 stored = checker[i];
        if (i < 256 * 4)
        {
            levelZeroMax = std::max(levelZeroMax, stored);
        }
        else
        {
            coarserMin = std::min(coarserMin, stored);
            coarserMax = std::max(coarserMax, stored);
        }
    }
    CHECK(levelZeroMax == 0);
    CHECK(coarserMin >= expected - 1);
    CHECK(coarserMax <= expected + 1);

    // Opting out drops the companion and its bytes.
    const vector<u8> optedOut = CookBlob(pack, CheckerNoVarianceId);
    CookedTextureHeader optedOutHeader{};
    std::memcpy(&optedOutHeader, optedOut.data(), sizeof(optedOutHeader));
    CHECK(optedOutHeader.VarianceFormat == 0u);
    CHECK(optedOut.size() == sizeof(CookedTextureHeader) + flat.size());
}

TEST_CASE("Normal variance: asking for a companion on a texture that is no normal map is refused")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path archive =
        Veng::TestSupport::TempDir() / "veng_cooker_color_normal_variance.vengpack";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult result =
        cooker.CookPack(fixtureDir / "texture_color_normal_variance_pack.json", archive);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("normal_variance") != string::npos);
    std::filesystem::remove(archive);
}
