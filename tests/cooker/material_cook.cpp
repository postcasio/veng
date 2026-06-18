// Material cook test: cooks fixture material_pack.json
// through libveng_cook and checks the resulting CookedMaterialHeader +
// CookedMaterialField table + single param block.
//
// material_data.slang:
//   struct MaterialParams {            // the one block (handles + params)
//     uint   Albedo;        // offset  0, size 4, Kind 1 (texture handle)
//     uint   AlbedoSampler; // offset  4, size 4, Kind 2 (sampler handle)
//     uint   Pad0;          // offset  8 — undeclared, not emitted as a field
//     uint   Pad1;          // offset 12 — undeclared, not emitted as a field
//     float4 Factors;       // offset 16, size 16, Kind 0 (param)
//   };                      // total 32 bytes (BlockBytes)
//
// Blob layout: CookedMaterialHeader → CookedMaterialField[3] → param block
// (32 bytes). The pads are zeroed in the block but carry no field-table entry.

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
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 3);
    CHECK(header.BlockBytes == 32);

    // Blob: header + 3 fields + 32-byte param block.
    const usize expectedSize = sizeof(CookedMaterialHeader)
        + 3 * sizeof(CookedMaterialField)
        + 32;
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

    // Field 2: Factors — Kind 0 (float param), block offset 16, Size 16
    CHECK(std::string_view(fieldTable[2].Name) == "Factors");
    CHECK(fieldTable[2].Kind == 0u);
    CHECK(fieldTable[2].Offset == 16u);
    CHECK(fieldTable[2].Size == 16u);
    CHECK(fieldTable[2].TextureId == 0ULL);

    // --- Param block: handle slots zeroed (the loader patches them) ---

    const u8* block = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + 3 * sizeof(CookedMaterialField);

    for (usize i = 0; i < 16; ++i)
        CHECK(block[i] == 0u);

    // --- Factors: four f32s at the block offset 16 ---

    f32 factors[4];
    std::memcpy(factors, block + 16, sizeof(factors));

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

TEST_CASE("Cooker: material cook fails when a params key has no matching MaterialParams field")
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

TEST_CASE("Cooker: material cook fails when a textures key has no matching MaterialParams field")
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
    // MaterialParams { uint Albedo; uint AlbedoSampler; uint Pad0; uint Pad1;
    // float4 Factors; float Roughness; } — std430: handle uints at 0..12,
    // Factors at 16, Roughness at 32.
    const path packJson = FixtureDir / "material_ext_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_ext.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3101});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.FieldCount == 4);
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.BlockBytes >= 36); // handles (16) + Factors (16) + Roughness (4)

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
    CHECK(factors->Offset == 16u);
    CHECK(roughness->Offset == 32u);

    const u8* block = entry->Blob.data()
        + sizeof(CookedMaterialHeader)
        + header.FieldCount * sizeof(CookedMaterialField);

    f32 roughnessVal = 0.0f;
    std::memcpy(&roughnessVal, block + roughness->Offset, sizeof(f32));
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
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.BlockBytes == 16);  // four handle uints

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    for (u32 i = 0; i < header.FieldCount; ++i)
        CHECK(fieldTable[i].Kind != 0u); // every field is a handle field

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a params-only material cooks with no handle fields")
{
    // The handle count is shader-driven, not a fixed engine struct: a material
    // may declare zero handle fields. params_only.frag has a MaterialParams of
    // only authored params (no Albedo/AlbedoSampler uints).
    const path packJson = FixtureDir / "material_params_only_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_params_only.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3103});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 2); // Factors + Strength, no handles
    CHECK(header.BlockBytes >= 20); // Factors (16) + Strength (4)

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    for (u32 i = 0; i < header.FieldCount; ++i)
        CHECK(fieldTable[i].Kind == 0u); // every field is a param, no handle field

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a multi-handle material cooks with two handle fields")
{
    // The handles-only material declares two handle fields (Albedo +
    // AlbedoSampler) — proving a handle count > 1 is shader-driven.
    const path packJson = FixtureDir / "material_handles_only_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_multi_handle.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3102});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 2);

    const CookedMaterialField* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    u32 handleFields = 0;
    for (u32 i = 0; i < header.FieldCount; ++i)
        if (fieldTable[i].Kind != 0u) ++handleFields;
    CHECK(handleFields == 2u);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: every cooked material carries the current format version")
{
    // The version field guards the format; the loader rejects a blob whose
    // Version != CookedMaterialVersion. A freshly cooked blob must stamp the
    // current version so a stale one is distinguishable.
    const path packJson = FixtureDir / "material_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_version.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xBB9});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a material with no domain key cooks as Surface (domain 0)")
{
    // The default domain is surface, so an existing material with no "domain" key
    // cooks with Domain == 0.
    const path packJson = FixtureDir / "material_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_domain_default.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xBB9});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 0u); // Surface

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a postprocess material cooks with domain 1")
{
    // A postprocess material declares "domain": "postprocess" and its fragment
    // shader writes a single float4 SV_Target0 — the postprocess output contract.
    const path packJson = FixtureDir / "material_postprocess_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_postprocess.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3201});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 1u); // PostProcess
    CHECK(header.FieldCount == 3); // Hdr + HdrSampler + Exposure

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: an unknown domain is a located cook error")
{
    const path packJson = FixtureDir / "material_bad_domain_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_bad_domain.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("unknown domain") != string::npos);
    CHECK(result.error().find("translucent") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a surface material whose fragment shader writes one target is a located cook error")
{
    // A surface material must write the g-buffer MRT (SV_Target0 + SV_Target1).
    // Pointing it at a shader that writes a single target is a contract mismatch.
    const path packJson = FixtureDir / "material_surface_wrong_output_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_surface_wrong.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("surface material must write the g-buffer") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a postprocess material whose fragment shader writes the MRT is a located cook error")
{
    // A postprocess material must write a single float4 SV_Target0. Pointing it at
    // a g-buffer (MRT) fragment shader is a contract mismatch.
    const path packJson = FixtureDir / "material_postprocess_wrong_output_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_material_postprocess_wrong.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("postprocess material must write a single") != string::npos);

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
