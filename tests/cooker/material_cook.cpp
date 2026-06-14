// Material cook test: cooks fixture material_pack.json
// through libveng_cook and checks the resulting CookedMaterialHeader +
// CookedMaterialField table + packed parameter block.
//
// MaterialData (material_data.slang):
//   struct MaterialData {
//     uint  Albedo;        // offset  0, size 4, Kind 1 (texture handle)
//     uint  AlbedoSampler; // offset  4, size 4, Kind 2 (sampler handle)
//     uint  Pad0;          // offset  8, size 4, Kind 0 (ignored pad)
//     uint  Pad1;          // offset 12, size 4, Kind 0 (ignored pad)
//     float4 Factors;      // offset 16, size 16, Kind 0 (param)
//   };                     // total 32 bytes
//
// Blob layout: CookedMaterialHeader → CookedMaterialField[5] → param block (32 bytes).

#include <cstring>
#include <filesystem>
#include <string_view>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    const path FixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);

    Result<ArchiveReader> CookMaterialPack(const path& packJson, const path& outArchive)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
        if (!cookResult.has_value())
            return std::unexpected(cookResult.error());

        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: cooks a material and validates the cooked blob layout")
{
    const path packJson = FixtureDir / "material_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    // --- Material entry ---

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3001});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Material);

    REQUIRE(entry->Blob.size() >= sizeof(CookedMaterialHeader));

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.VertexShaderId == 4101ULL);
    CHECK(header.FragmentShaderId == 4102ULL);
    CHECK(header.FieldCount == 5);
    CHECK(header.ParamBytes == 32);

    // Blob must be large enough for header + 5 fields + 32-byte param block.
    const usize expectedSize = sizeof(CookedMaterialHeader)
        + 5 * sizeof(CookedMaterialField)
        + 32;
    REQUIRE(entry->Blob.size() == expectedSize);

    // --- Field table ---

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // Field 0: Albedo — Kind 1 (texture handle), Offset 0, Size 4, TextureId 2001
    CHECK(std::string_view(fieldTable[0].Name) == "Albedo");
    CHECK(fieldTable[0].Kind == 1u);
    CHECK(fieldTable[0].Offset == 0u);
    CHECK(fieldTable[0].Size == 4u);
    CHECK(fieldTable[0].TextureId == 2001ULL);

    // Field 1: AlbedoSampler — Kind 2 (sampler handle), Offset 4, Size 4, TextureId 2001
    CHECK(std::string_view(fieldTable[1].Name) == "AlbedoSampler");
    CHECK(fieldTable[1].Kind == 2u);
    CHECK(fieldTable[1].Offset == 4u);
    CHECK(fieldTable[1].Size == 4u);
    CHECK(fieldTable[1].TextureId == 2001ULL);

    // Field 2: Pad0 — Kind 0 (ignored pad), Offset 8, Size 4
    CHECK(std::string_view(fieldTable[2].Name) == "Pad0");
    CHECK(fieldTable[2].Kind == 0u);
    CHECK(fieldTable[2].Offset == 8u);
    CHECK(fieldTable[2].Size == 4u);
    CHECK(fieldTable[2].TextureId == 0ULL);

    // Field 3: Pad1 — Kind 0, Offset 12, Size 4
    CHECK(std::string_view(fieldTable[3].Name) == "Pad1");
    CHECK(fieldTable[3].Kind == 0u);
    CHECK(fieldTable[3].Offset == 12u);
    CHECK(fieldTable[3].Size == 4u);
    CHECK(fieldTable[3].TextureId == 0ULL);

    // Field 4: Factors — Kind 0 (float param), Offset 16, Size 16
    CHECK(std::string_view(fieldTable[4].Name) == "Factors");
    CHECK(fieldTable[4].Kind == 0u);
    CHECK(fieldTable[4].Offset == 16u);
    CHECK(fieldTable[4].Size == 16u);
    CHECK(fieldTable[4].TextureId == 0ULL);

    // --- Param block: four f32s at offset 16 within the block ---

    const u8* paramBlock = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + 5 * sizeof(CookedMaterialField);

    f32 factors[4];
    std::memcpy(factors, paramBlock + 16, sizeof(factors));

    CHECK(factors[0] == doctest::Approx(1.0f));
    CHECK(factors[1] == doctest::Approx(0.9f));
    CHECK(factors[2] == doctest::Approx(0.8f));
    CHECK(factors[3] == doctest::Approx(1.0f));

    // Pad slots should be zero in the param block.
    u32 pad0Val = 0, pad1Val = 0;
    std::memcpy(&pad0Val, paramBlock + 8,  sizeof(u32));
    std::memcpy(&pad1Val, paramBlock + 12, sizeof(u32));
    CHECK(pad0Val == 0u);
    CHECK(pad1Val == 0u);

    // --- Referenced shader and texture entries are also present ---

    CHECK(reader->Find(AssetId{4101}).has_value()); // vertex shader
    CHECK(reader->Find(AssetId{4102}).has_value()); // fragment shader
    CHECK(reader->Find(AssetId{2001}).has_value()); // texture

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: material cook fails when a params key has no matching MaterialData field")
{
    const path packJson = FixtureDir / "material_bad_param.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_bad_param.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    CHECK(!result.has_value());
    // The error message must name the bad key.
    REQUIRE(!result.has_value());
    CHECK(result.error().find("Nonexistent") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: material cook fails when a textures key has no matching MaterialData field")
{
    const path packJson = FixtureDir / "material_bad_texture.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_bad_texture.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    CHECK(!result.has_value());
    REQUIRE(!result.has_value());
    CHECK(result.error().find("Missing") != string::npos);

    std::filesystem::remove(outArchive);
}
