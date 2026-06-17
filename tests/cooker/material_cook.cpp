// Material cook test: cooks fixture material_pack.json
// through libveng_cook and checks the resulting CookedMaterialHeader +
// CookedMaterialField table + engine block + authored block.
//
// material_data.slang:
//   struct MaterialData {              // the engine block (handle slots)
//     uint  Albedo;        // offset  0, size 4, Kind 1 (texture handle)
//     uint  AlbedoSampler; // offset  4, size 4, Kind 2 (sampler handle)
//     uint  Pad0;          // offset  8 — undeclared, not emitted as a field
//     uint  Pad1;          // offset 12 — undeclared, not emitted as a field
//   };                     // total 16 bytes (EngineBytes)
//   struct MaterialParams {            // the authored block
//     float4 Factors;      // offset  0, size 16, Kind 0 (param)
//   };                     // total 16 bytes (ParamBytes)
//
// Blob layout: CookedMaterialHeader → CookedMaterialField[3] → engine block
// (16 bytes) → authored block (16 bytes). The pads are zeroed in the engine
// block but carry no field-table entry.

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

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xBB9});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Material);

    REQUIRE(entry->Blob.size() >= sizeof(CookedMaterialHeader));

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.VertexShaderId == 4101ULL);
    CHECK(header.FragmentShaderId == 4102ULL);
    CHECK(header.FieldCount == 3);
    CHECK(header.EngineBytes == 16);
    CHECK(header.ParamBytes == 16);

    // Blob: header + 3 fields + 16-byte engine block + 16-byte authored block.
    const usize expectedSize = sizeof(CookedMaterialHeader)
        + 3 * sizeof(CookedMaterialField)
        + 16
        + 16;
    REQUIRE(entry->Blob.size() == expectedSize);

    // --- Field table: only the 3 declared fields, pads omitted ---

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // Field 0: Albedo — Kind 1 (texture handle), engine offset 0, Size 4, TextureId 2001
    CHECK(std::string_view(fieldTable[0].Name) == "Albedo");
    CHECK(fieldTable[0].Kind == 1u);
    CHECK(fieldTable[0].Offset == 0u);
    CHECK(fieldTable[0].Size == 4u);
    CHECK(fieldTable[0].TextureId == 2001ULL);

    // Field 1: AlbedoSampler — Kind 2 (sampler handle), engine offset 4, Size 4, TextureId 2001
    CHECK(std::string_view(fieldTable[1].Name) == "AlbedoSampler");
    CHECK(fieldTable[1].Kind == 2u);
    CHECK(fieldTable[1].Offset == 4u);
    CHECK(fieldTable[1].Size == 4u);
    CHECK(fieldTable[1].TextureId == 2001ULL);

    // Field 2: Factors — Kind 0 (float param), authored offset 0, Size 16
    CHECK(std::string_view(fieldTable[2].Name) == "Factors");
    CHECK(fieldTable[2].Kind == 0u);
    CHECK(fieldTable[2].Offset == 0u);
    CHECK(fieldTable[2].Size == 16u);
    CHECK(fieldTable[2].TextureId == 0ULL);

    // --- Engine block: handle slots zeroed (the loader patches them) ---

    const u8* engineBlock = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + 3 * sizeof(CookedMaterialField);

    for (usize i = 0; i < 16; ++i)
        CHECK(engineBlock[i] == 0u);

    // --- Authored block: four f32s of Factors at offset 0 ---

    const u8* authoredBlock = engineBlock + 16;

    f32 factors[4];
    std::memcpy(factors, authoredBlock, sizeof(factors));

    CHECK(factors[0] == doctest::Approx(1.0f));
    CHECK(factors[1] == doctest::Approx(0.9f));
    CHECK(factors[2] == doctest::Approx(0.8f));
    CHECK(factors[3] == doctest::Approx(1.0f));

    // --- Referenced shader and texture entries are also present ---

    CHECK(reader->Find(AssetId{0x1005}).has_value()); // vertex shader
    CHECK(reader->Find(AssetId{0x1006}).has_value()); // fragment shader
    CHECK(reader->Find(AssetId{0x7D1}).has_value()); // texture

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

TEST_CASE("Cooker: an authored param beyond Factors cooks into the authored block")
{
    // MaterialParams { float4 Factors; float Roughness; } — std430: Factors at
    // offset 0, Roughness at 16; struct size 32 (the trailing pad to 16-byte
    // alignment of the array stride is not a member offset).
    const path packJson = FixtureDir / "material_ext_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_ext.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3101});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.FieldCount == 4);
    CHECK(header.EngineBytes == 16);
    CHECK(header.ParamBytes >= 20); // Factors (16) + Roughness (4)

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // Find the Roughness field: a Kind 0 authored param.
    const CookedMaterialField* roughness = nullptr;
    const CookedMaterialField* factors = nullptr;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        if (std::string_view(fieldTable[i].Name) == "Roughness") roughness = &fieldTable[i];
        if (std::string_view(fieldTable[i].Name) == "Factors")   factors   = &fieldTable[i];
    }
    REQUIRE(roughness != nullptr);
    REQUIRE(factors != nullptr);
    CHECK(roughness->Kind == 0u);
    CHECK(roughness->Size == 4u);
    CHECK(factors->Offset == 0u);
    CHECK(roughness->Offset == 16u);

    const u8* authoredBlock = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + header.FieldCount * sizeof(CookedMaterialField)
        + header.EngineBytes;

    f32 roughnessVal = 0.0f;
    std::memcpy(&roughnessVal, authoredBlock + roughness->Offset, sizeof(f32));
    CHECK(roughnessVal == doctest::Approx(0.25f));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a handles-only material cooks with a zero-size authored block")
{
    const path packJson = FixtureDir / "material_handles_only_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_handles_only.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3102});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.FieldCount == 2);   // Albedo + AlbedoSampler, no params
    CHECK(header.EngineBytes == 16);
    CHECK(header.ParamBytes == 0);

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    for (u32 i = 0; i < header.FieldCount; ++i)
        CHECK(fieldTable[i].Kind != 0u); // every field is a handle field

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: an authored block exceeding the param stride is a located cook error")
{
    const path packJson = FixtureDir / "material_oversize_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_oversize.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("exceeds stride") != string::npos);

    std::filesystem::remove(outArchive);
}
