// Rumble-clip cook test: cooks a *.rumble.json through the RumbleClipImporter, checks the
// CookedRumbleClipHeader and that the record round-trips through ReadFields, and covers each rule
// the cook enforces — unsorted keys, a value outside [0, 1], a time past the duration, a zero
// duration, no keyed channel and an unknown key — each failing with the asset id in the message. A
// clip needs no --module, so the cook runs with a builtin-only registry.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include <doctest/doctest.h>
#include <fmt/format.h>

#include "support/TempPath.h"

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr u64 ClipId = 0x0000000000004D2FULL;

    // Cooks a one-clip pack built around @p clip and returns the blob, or the cook's error.
    Result<vector<u8>> CookClip(const json& clip)
    {
        std::random_device rng;
        const string stem = fmt::format("veng_cooker_rumble_{:08x}", rng());
        const path dir = Veng::TestSupport::TempDir();
        const path clipPath = dir / (stem + ".rumble.json");
        const path packPath = dir / (stem + ".pack.json");
        const path archive = dir / (stem + ".vengpack");

        std::ofstream(clipPath) << clip.dump();
        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({{{"id", FormatHexId(ClipId)},
                                       {"type", "RumbleClip"},
                                       {"source", clipPath.filename().string()}}});
        std::ofstream(packPath) << pack.dump();

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const VoidResult cooked = cooker.CookPack(packPath, archive);
        std::filesystem::remove(clipPath);
        std::filesystem::remove(packPath);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(AssetId{ClipId});
        REQUIRE(entry.has_value());
        vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(archive);
        return blob;
    }

    json Key(const f32 time, const f32 value, const char* interp = "Linear")
    {
        return json{{"Time", time}, {"Value", value}, {"Interp", interp}};
    }

    json Pulse()
    {
        return json{
            {"Duration", 0.5},
            {"Loop", true},
            {"LowFrequency", {{"Keys", json::array({Key(0.0f, 1.0f, "Smooth"), Key(0.5f, 0.0f)})}}},
            {"RightTrigger", {{"Keys", json::array({Key(0.1f, 0.5f, "Step")})}}}};
    }

    // Cooks @p clip expecting failure, and checks the message names the asset and @p fragment.
    void CheckRejected(const json& clip, const string& fragment)
    {
        const Result<vector<u8>> cooked = CookClip(clip);
        REQUIRE_FALSE(cooked.has_value());
        INFO(cooked.error());
        CHECK(cooked.error().find(FormatHexId(ClipId)) != string::npos);
        CHECK(cooked.error().find(fragment) != string::npos);
    }
}

TEST_CASE("rumble clip cook: a clip round-trips through the header and record")
{
    const Result<vector<u8>> blob = CookClip(Pulse());
    REQUIRE(blob.has_value());
    REQUIRE(blob->size() >= sizeof(CookedRumbleClipHeader));

    CookedRumbleClipHeader header{};
    std::memcpy(&header, blob->data(), sizeof(header));
    CHECK(header.Version == CookedRumbleClipVersion);
    REQUIRE(blob->size() == sizeof(header) + header.RecordBytes);

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    Haptics::RumbleClipData clip;
    const VoidResult read =
        ReadFields(std::span<const u8>(blob->data() + sizeof(header), header.RecordBytes), &clip,
                   registry.Info(TypeIdOf<Haptics::RumbleClipData>()), registry);
    REQUIRE(read.has_value());

    CHECK(clip.Duration == doctest::Approx(0.5f));
    CHECK(clip.Loop);
    REQUIRE(clip.LowFrequency.Keys.size() == 2);
    CHECK(clip.LowFrequency.Keys[0].Interp == CurveInterp::Smooth);
    CHECK(clip.HighFrequency.IsEmpty());
    REQUIRE(clip.RightTrigger.Keys.size() == 1);
    CHECK(clip.RightTrigger.Keys[0].Interp == CurveInterp::Step);
}

TEST_CASE("rumble clip cook: the cook rejects what would not play")
{
    json unsorted = Pulse();
    unsorted["LowFrequency"]["Keys"] = json::array({Key(0.4f, 1.0f), Key(0.2f, 0.0f)});
    CheckRejected(unsorted, "key 1");

    json loud = Pulse();
    loud["LowFrequency"]["Keys"] = json::array({Key(0.0f, 1.5f)});
    CheckRejected(loud, "outside [0, 1]");

    json late = Pulse();
    late["RightTrigger"]["Keys"] = json::array({Key(0.75f, 0.5f)});
    CheckRejected(late, "RightTrigger");

    json instant = Pulse();
    instant["Duration"] = 0.0;
    CheckRejected(instant, "Duration");

    CheckRejected(json{{"Duration", 1.0}}, "every channel is empty");

    json typo = Pulse();
    typo["Lowfrequency"] = typo["LowFrequency"];
    CheckRejected(typo, "Lowfrequency");
}
