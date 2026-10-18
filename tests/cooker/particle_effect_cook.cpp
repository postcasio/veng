// Particle-effect cook: a two-emitter, one-flash effect — a billboard over a flipbook cooked beside
// it, and a streak — cooks to a header with the stated counts and the emitters' fields, and a
// handful of representative malformations are refused by a message naming the field.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId EffectId{0x421038534CAE54E4ULL};
    constexpr AssetId FlipbookId{0xCFEB68B2EDB6B1A1ULL};
    const string FlipbookHex = fmt::format("0x{:016X}", FlipbookId.Value);

    // An 8x8 white uncompressed TGA, standing in for a baked sheet.
    void WriteAtlas(const path& file)
    {
        std::ofstream out(file, std::ios::binary);
        const u8 header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 8, 0, 32, 0x28};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const u8 white[4] = {255, 255, 255, 255};
        for (u32 texel = 0; texel < 64; ++texel)
        {
            out.write(reinterpret_cast<const char*>(white), sizeof(white));
        }
    }

    nlohmann::json Atlas()
    {
        return {
            {"version", 1},
            {"type", "flipbook-atlas"},
            {"atlas", {{"file", "sheet.tga"}, {"width", 8}, {"height", 8}}},
            {"frame", {{"width", 4}, {"height", 4}}},
            {"columns", 2},
            {"rows", 2},
            {"frames", 4},
            {"fps", 10.0},
            {"loop", false},
            {"blend", "alpha"},
            {"alpha", "premultiplied"},
            {"colorSpace", "srgb"},
        };
    }

    // A billboard over the flipbook beside it and a streak, with one flash.
    nlohmann::json Effect()
    {
        return {
            {"type", "particle-effect"},
            {"version", 1},
            {"emitters",
             {
                 {{"name", "smoke"},
                  {"render", "billboard"},
                  {"flipbook", FlipbookHex},
                  {"spawn", {{"burst", {{"count", 48}, {"time", 0.0}}}}},
                  {"lifetime", {1.0, 2.0}},
                  {"shape", {{"kind", "sphere"}, {"radius", 0.5}}},
                  {"velocity", {{"speed", {2.0, 4.0}}, {"speedByRadius", 1.0}}},
                  {"overLife",
                   {{"size", {{0.0, 0.5}, {1.0, 2.0}}},
                    {"tint", {{0.0, {1.0, 0.8, 0.6}}, {1.0, {0.2, 0.2, 0.2}}}}}}},
                 {{"name", "sparks"},
                  {"render", "streak"},
                  {"blend", "additive"},
                  {"spawn", {{"rate", {{"perSecond", 120.0}, {"start", 0.0}, {"duration", 0.25}}}}},
                  {"velocity",
                   {{"speed", {20.0, 40.0}}, {"cone", {{"axis", {0, 1, 0}}, {"angle", 60}}}}},
                  {"drag", 1.5},
                  {"streak", {{"color", {8.0, 5.0, 2.0}}, {"lengthScale", 0.03}}}},
             }},
            {"flashes",
             {{{"time", 0.0}, {"duration", 0.15}, {"intensity", 50000.0}, {"range", 60.0}}}},
        };
    }

    struct Cooked
    {
        VoidResult Status;
        vector<u8> Blob;
    };

    // Cooks the flipbook and `effect` in a fresh directory, returning the effect's blob.
    Cooked CookEffect(const string& tag, const nlohmann::json& effect)
    {
        const path dir = Veng::TestSupport::TempDir() / fmt::format("particle_effect_cook_{}", tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        WriteAtlas(dir / "sheet.tga");
        std::ofstream(dir / "sheet.atlas.json") << Atlas().dump(2);
        std::ofstream(dir / "burst.particles.json") << effect.dump(2);
        const nlohmann::json pack = {
            {"version", 1},
            {"assets",
             {{{"id", FlipbookHex}, {"type", "Flipbook"}, {"source", "sheet.atlas.json"}},
              {{"id", fmt::format("0x{:016X}", EffectId.Value)},
               {"type", "ParticleEffect"},
               {"source", "burst.particles.json"}}}},
        };
        std::ofstream(dir / "pack.json") << pack.dump(2);

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const path archive = dir / "out.vengpack";
        Cooked cooked{.Status = cooker.CookPack(dir / "pack.json", archive, {})};
        if (!cooked.Status)
        {
            return cooked;
        }
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(EffectId);
        REQUIRE(entry.has_value());
        cooked.Blob.assign(entry->Blob.begin(), entry->Blob.end());
        return cooked;
    }

    // Cooks a malformed effect and returns the refusal.
    string Refusal(const string& tag, const nlohmann::json& effect)
    {
        const Cooked cooked = CookEffect(tag, effect);
        REQUIRE_FALSE(cooked.Status.has_value());
        return cooked.Status.error();
    }
}

TEST_CASE("particle effect cook: two emitters and a flash cook to the stated counts")
{
    const Cooked cooked = CookEffect("valid", Effect());
    REQUIRE(cooked.Status.has_value());
    REQUIRE(cooked.Blob.size() >= sizeof(CookedParticleEffectHeader));
    CookedParticleEffectHeader header;
    std::memcpy(&header, cooked.Blob.data(), sizeof(header));
    CHECK(header.Version == CookedParticleEffectVersion);
    CHECK(header.EmitterCount == 2);
    CHECK(header.FlashCount == 1);
    // Two size keys (t, v) and two tint keys (t, r, g, b).
    CHECK(header.KeyFloats == 2 * 2 + 2 * 4);
    CHECK(cooked.Blob.size() == sizeof(header) + 2 * sizeof(CookedParticleEmitter) +
                                    sizeof(CookedParticleFlash) + header.KeyFloats * sizeof(f32));

    CookedParticleEmitter smoke;
    CookedParticleEmitter sparks;
    std::memcpy(&smoke, cooked.Blob.data() + sizeof(header), sizeof(smoke));
    std::memcpy(&sparks, cooked.Blob.data() + sizeof(header) + sizeof(smoke), sizeof(sparks));
    CHECK(smoke.Flipbook == FlipbookId.Value);
    CHECK(smoke.BurstCount == 48);
    CHECK(smoke.Shape == 1);
    CHECK(smoke.SizeCurve.KeyCount == 2);
    CHECK(smoke.TintCurve.KeyCount == 2);
    CHECK(sparks.Flipbook == 0);
    CHECK(sparks.Render == 1);
    CHECK(sparks.Blend == 2);
    CHECK(sparks.RatePerSecond == 120.0f);
    CHECK(sparks.Drag == 1.5f);
    CHECK(sparks.ConeAngle == 60.0f);
}

TEST_CASE("particle effect cook: a malformed effect is refused naming the field")
{
    {
        nlohmann::json effect = Effect();
        effect["version"] = 2;
        CHECK(Refusal("version", effect).find("version") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][1]["render"] = "ribbon";
        CHECK(Refusal("render", effect).find("emitters[1].render") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][0]["flipbook"] = fmt::format("0x{:016X}", EffectId.Value);
        CHECK(Refusal("flipbook", effect).find("emitters[0].flipbook") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][1]["blend"] = "asset";
        CHECK(Refusal("streakblend", effect).find("emitters[1].blend") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][0]["lifetime"] = {2.0, 1.0};
        CHECK(Refusal("range", effect).find("emitters[0].lifetime") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][1]["velocity"]["cone"]["angle"] = 0;
        CHECK(Refusal("cone", effect).find("emitters[1].velocity.cone.angle") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][0]["overLife"]["size"] = {{0.5, 1.0}, {0.25, 2.0}};
        CHECK(Refusal("keys", effect).find("emitters[0].overLife.size[1]") != string::npos);
    }
    {
        nlohmann::json effect = Effect();
        effect["emitters"][0]["spawn"] = nlohmann::json::object();
        CHECK(Refusal("nothing", effect).find("emitters[0].spawn") != string::npos);
    }
}
