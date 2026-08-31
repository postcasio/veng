// Material cook test: cooks fixture material_pack.json
// through libveng_cook and checks the resulting CookedMaterialHeader +
// CookedMaterialField table + single param block.
//
// The fixture fragment (brick.frag.slang) declares:
//   struct MaterialParams {            // the one block (params + handles)
//     float4 Factors;       // offset  0, size 16, Kind 0 (param)
//     uint   Albedo;        // offset 16, size  4, Kind 1 (texture handle)
//     uint   AlbedoSampler; // offset 20, size  4, Kind 2 (sampler handle)
//   };                      // total 24 bytes (BlockBytes)
//
// The block is packed in scalar/tight layout (the layout the shader's Load<T> reads), so
// the offsets are contiguous and BlockBytes is the tight 24, not a 16-rounded 32.
//
// Blob layout: CookedMaterialHeader → CookedMaterialField[3] → param block (24 bytes).

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"
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
        {
            return std::unexpected(cookResult.error());
        }

        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: cooks a material and validates the cooked blob layout")
{
    const path packJson = FixtureDir / "material_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    // --- Material entry ---

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xBB9});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Material);

    REQUIRE(entry->Blob.size() >= sizeof(CookedMaterialHeader));

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.VertexShaderId == 4101ULL);
    CHECK(header.FragmentShaderId == 4102ULL);
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 3);
    CHECK(header.BlockBytes == 24);

    // Blob: header + 3 fields + 24-byte param block. The fixture's MaterialParams leads with its
    // float4 and follows with the two handle uints — 16 + 4 + 4 of members, tightly packed with no
    // hole between them and no rounded tail: scalar/tight layout is exactly the members' span.
    const usize expectedSize = sizeof(CookedMaterialHeader) + 3 * sizeof(CookedMaterialField) + 24;
    REQUIRE(entry->Blob.size() == expectedSize);

    // --- Field table: the 3 declared fields, at the offsets reflection gives them ---

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // Field 0: Albedo — Kind 1 (texture handle), engine offset 16, Size 4, TextureId 2001. The
    // table is in the material's declaration order; the offsets are the shader's.
    CHECK(std::string_view(fieldTable[0].Name) == "Albedo");
    CHECK(fieldTable[0].Kind == 1u);
    CHECK(fieldTable[0].Offset == 16u);
    CHECK(fieldTable[0].Size == 4u);
    CHECK(fieldTable[0].TextureId == 2001ULL);

    // Field 1: AlbedoSampler — Kind 2 (sampler handle), engine offset 20, Size 4, TextureId 2001
    CHECK(std::string_view(fieldTable[1].Name) == "AlbedoSampler");
    CHECK(fieldTable[1].Kind == 2u);
    CHECK(fieldTable[1].Offset == 20u);
    CHECK(fieldTable[1].Size == 4u);
    CHECK(fieldTable[1].TextureId == 2001ULL);

    // Field 2: Factors — Kind 0 (float param), block offset 0, Size 16
    CHECK(std::string_view(fieldTable[2].Name) == "Factors");
    CHECK(fieldTable[2].Kind == 0u);
    CHECK(fieldTable[2].Offset == 0u);
    CHECK(fieldTable[2].Size == 16u);
    CHECK(fieldTable[2].TextureId == 0ULL);

    // --- Param block: handle slots zeroed (the loader patches them) ---

    const u8* block =
        entry->Blob.data() + sizeof(CookedMaterialHeader) + 3 * sizeof(CookedMaterialField);

    // The two handle slots (Albedo at 16, AlbedoSampler at 20) are zeroed; the loader patches them.
    for (usize i = 16; i < 24; ++i)
    {
        CHECK(block[i] == 0u);
    }

    // --- Factors: four f32s at the block offset 0 ---

    f32 factors[4];
    std::memcpy(factors, block, sizeof(factors));

    CHECK(factors[0] == doctest::Approx(1.0f));
    CHECK(factors[1] == doctest::Approx(0.9f));
    CHECK(factors[2] == doctest::Approx(0.8f));
    CHECK(factors[3] == doctest::Approx(1.0f));

    // --- Referenced shader and texture entries are also present ---

    CHECK(reader->Find(AssetId{0x1005}).has_value()); // vertex shader
    CHECK(reader->Find(AssetId{0x1006}).has_value()); // fragment shader
    CHECK(reader->Find(AssetId{0x7D1}).has_value());  // texture

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: material cook fails when a params key has no matching MaterialParams field")
{
    const path packJson = FixtureDir / "material_bad_param.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_bad_param.vengpack";

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
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_bad_texture.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    CHECK(!result.has_value());
    REQUIRE(!result.has_value());
    CHECK(result.error().find("Missing") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: material cook fails when the shader declares a field the material does not")
{
    // The other half of the pairing, and the half that used to fail silently. A field the material
    // declares and the shader lacks has always been an error. A field the *shader* declares and the
    // material omits was never reached at all: it stayed zero in the block image, cooked clean, and
    // the fragment read zero for the life of the asset — and where zero is not inert for that
    // parameter (a scale, a radius, a count, a coverage) the surface simply drew wrong with nothing
    // anywhere naming the missing line.
    //
    // The rule is now total: every member of MaterialParams has a "fields" entry, with no exemption
    // for padding, because a struct that needs padding can be ordered so it does not. The fixture
    // material declares the two handles and omits the Factors its fragment carries.
    const path packJson = FixtureDir / "material_missing_field.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_missing_field.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    // The error names the member left undeclared, which is the whole of what an author needs.
    CHECK(result.error().find("Factors") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: an authored param beyond Factors cooks into the authored block")
{
    // MaterialParams { float4 Factors; float Roughness; uint Albedo; uint AlbedoSampler; } —
    // scalar/tight: Factors at 0, Roughness at 16, the two handles at 20 and 24; BlockBytes 28.
    const path packJson = FixtureDir / "material_ext_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material_ext.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3101});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.FieldCount == 4);
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.BlockBytes >= 28); // Factors (16) + Roughness (4) + two handles (8)

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // Find the Roughness field: a Kind 0 authored param.
    const CookedMaterialField* roughness = nullptr;
    const CookedMaterialField* factors = nullptr;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        if (std::string_view(fieldTable[i].Name) == "Roughness")
        {
            roughness = &fieldTable[i];
        }
        if (std::string_view(fieldTable[i].Name) == "Factors")
        {
            factors = &fieldTable[i];
        }
    }
    REQUIRE(roughness != nullptr);
    REQUIRE(factors != nullptr);
    CHECK(roughness->Kind == 0u);
    CHECK(roughness->Size == 4u);
    CHECK(factors->Offset == 0u);
    CHECK(roughness->Offset == 16u);

    const u8* block = entry->Blob.data() + sizeof(CookedMaterialHeader) +
                      header.FieldCount * sizeof(CookedMaterialField);

    f32 roughnessVal = 0.0f;
    std::memcpy(&roughnessVal, block + roughness->Offset, sizeof(f32));
    CHECK(roughnessVal == doctest::Approx(0.25f));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a handles-only material cooks with a zero-size authored block")
{
    const path packJson = FixtureDir / "material_handles_only_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_handles_only.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3102});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.FieldCount == 2); // Albedo + AlbedoSampler, no params
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.BlockBytes == 8); // two handle uints, tightly packed

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        CHECK(fieldTable[i].Kind != 0u); // every field is a handle field
    }

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a params-only material cooks with no handle fields")
{
    // The handle count is shader-driven, not a fixed engine struct: a material
    // may declare zero handle fields. params_only.frag has a MaterialParams of
    // only authored params (no Albedo/AlbedoSampler uints).
    const path packJson = FixtureDir / "material_params_only_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_params_only.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3103});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 2);  // Factors + Strength, no handles
    CHECK(header.BlockBytes >= 20); // Factors (16) + Strength (4)

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        CHECK(fieldTable[i].Kind == 0u); // every field is a param, no handle field
    }

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a vector after a scalar packs at its tight offset, not a 16-aligned one")
{
    // The mispack regression. A MaterialParams that places a vector after a scalar is read
    // shader-side by g_MaterialParams.Load<MaterialParams>() in scalar/tight layout — every field
    // 4-byte packed, vectors not 16-aligned. The cooker must reflect that same layout, so the
    // offset it packs a value at is the offset the shader reads it from. Under the old std140
    // reflection the vectors 16-aligned: Mid would sit at 16 (not 4) and Region at 32 (not 16),
    // and the write side and read side disagreed, so a vector after a scalar read garbage.
    //
    // struct MaterialParams { float Lead; float3 Mid; float4 Region; uint Count; } packs tight to
    // Lead@0, Mid@4, Region@16, Count@32; BlockBytes 36.
    const path packJson = FixtureDir / "material_scalar_before_vec_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_scalar_before_vec.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xC21});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 4);
    CHECK(header.BlockBytes == 36);

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    const CookedMaterialField* lead = nullptr;
    const CookedMaterialField* mid = nullptr;
    const CookedMaterialField* region = nullptr;
    const CookedMaterialField* count = nullptr;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        const std::string_view name(fieldTable[i].Name);
        if (name == "Lead")
        {
            lead = &fieldTable[i];
        }
        else if (name == "Mid")
        {
            mid = &fieldTable[i];
        }
        else if (name == "Region")
        {
            region = &fieldTable[i];
        }
        else if (name == "Count")
        {
            count = &fieldTable[i];
        }
    }
    REQUIRE(lead != nullptr);
    REQUIRE(mid != nullptr);
    REQUIRE(region != nullptr);
    REQUIRE(count != nullptr);

    // The tight offsets Load<T> reads — the whole point of the fix.
    CHECK(lead->Offset == 0u);
    CHECK(lead->Size == 4u);
    CHECK(mid->Offset == 4u);
    CHECK(mid->Size == 12u);
    CHECK(region->Offset == 16u);
    CHECK(region->Size == 16u);
    CHECK(count->Offset == 32u);
    CHECK(count->Size == 4u);

    // The authored values land at those offsets in the cooked block.
    const u8* block = entry->Blob.data() + sizeof(CookedMaterialHeader) +
                      header.FieldCount * sizeof(CookedMaterialField);

    f32 lead0 = 0.0f;
    std::memcpy(&lead0, block + lead->Offset, sizeof(f32));
    CHECK(lead0 == doctest::Approx(3.0f));

    f32 midv[3];
    std::memcpy(midv, block + mid->Offset, sizeof(midv));
    CHECK(midv[0] == doctest::Approx(0.1f));
    CHECK(midv[1] == doctest::Approx(0.2f));
    CHECK(midv[2] == doctest::Approx(0.3f));

    f32 regionv[4];
    std::memcpy(regionv, block + region->Offset, sizeof(regionv));
    CHECK(regionv[0] == doctest::Approx(0.25f));
    CHECK(regionv[1] == doctest::Approx(0.5f));
    CHECK(regionv[2] == doctest::Approx(0.75f));
    CHECK(regionv[3] == doctest::Approx(1.0f));

    u32 countv = 0;
    std::memcpy(&countv, block + count->Offset, sizeof(u32));
    CHECK(countv == 7u);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a multi-handle material cooks with two handle fields")
{
    // The handles-only material declares two handle fields (Albedo +
    // AlbedoSampler) — proving a handle count > 1 is shader-driven.
    const path packJson = FixtureDir / "material_handles_only_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_multi_handle.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3102});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.FieldCount == 2);

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));
    u32 handleFields = 0;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        if (fieldTable[i].Kind != 0u)
        {
            ++handleFields;
        }
    }
    CHECK(handleFields == 2u);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: every cooked material carries the current format version")
{
    // The version field guards the format; the loader rejects a blob whose
    // Version != CookedMaterialVersion. A freshly cooked blob must stamp the
    // current version so a stale one is distinguishable.
    const path packJson = FixtureDir / "material_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material_version.vengpack";

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
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_domain_default.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xBB9});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 0u);   // Surface
    CHECK(header.CullMode == 2u); // Back — the default for a material with no "cull" key

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a postprocess material cooks with domain 1")
{
    // A PostProcess material declares "domain": "PostProcess" and its fragment
    // shader writes a single float4 SV_Target0 — the postprocess output contract.
    const path packJson = FixtureDir / "material_postprocess_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_postprocess.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{3201});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 1u);    // PostProcess
    CHECK(header.FieldCount == 3); // Hdr + HdrSampler + Exposure

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a sky material cooks with domain 2 and a storage-buffer handle field")
{
    // A Sky material declares "domain": "Sky" and its fragment writes a single float4
    // SV_Target0 (radiance). It also declares a "storagebuffer" field, which cooks as a
    // runtime-bound handle (Kind 3, TextureId 0).
    const path packJson = FixtureDir / "material_sky_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material_sky.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xC84});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 2u);    // Sky
    CHECK(header.FieldCount == 3); // Tint + Points + Count

    const auto* fieldTable = reinterpret_cast<const CookedMaterialField*>(
        entry->Blob.data() + sizeof(CookedMaterialHeader));

    // The Points field cooks as a Kind 3 storage-buffer handle with no cooked asset.
    const CookedMaterialField* points = nullptr;
    for (u32 i = 0; i < header.FieldCount; ++i)
    {
        if (std::string_view(fieldTable[i].Name) == "Points")
        {
            points = &fieldTable[i];
        }
    }
    REQUIRE(points != nullptr);
    CHECK(points->Kind == 3u);
    CHECK(points->Size == 4u);
    CHECK(points->TextureId == 0ULL);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a sky material whose fragment writes the MRT is a located cook error")
{
    // A Sky material must write a single float4 SV_Target0 (background radiance), not the
    // g-buffer MRT. Pointing it at a g-buffer fragment shader violates the radiance contract.
    const path packJson = FixtureDir / "material_sky_wrong_output_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_sky_wrong.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("sky material must write a single float4 SV_Target0") !=
          string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a translucent material cooks with domain 3")
{
    // A Translucent material declares "domain": "Translucent" and its fragment writes a single
    // float4 SV_Target0 (final HDR color + alpha). It cooks with domain 3 like any other material.
    const path packJson = FixtureDir / "material_translucent_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_translucent.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xC86});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 3u);    // Translucent
    CHECK(header.CullMode == 0u);  // None — the fixture authors "cull": "None"
    CHECK(header.FieldCount == 1); // Color

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a fragment writing SV_Depth beside its target cooks")
{
    // SV_Depth names the depth attachment, not a color one, so it is dropped from the reflected
    // target set and the domain still sees exactly its own targets. The fixture's fragment
    // returns a struct of float4 SV_Target0 + float SV_Depth against the single-target
    // Translucent contract, which would fail the count check if the depth member were collected.
    const path packJson = FixtureDir / "material_translucent_depth_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_translucent_depth.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xC87});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Domain == 3u);    // Translucent
    CHECK(header.FieldCount == 1); // Color

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a half-resolution translucent material cooks with the flag set")
{
    // "resolution": "half" opts a Translucent material into the reduced-resolution translucent
    // layer; the cooked header carries the flag the loader threads to Material::IsHalfResolution.
    const path packJson = FixtureDir / "material_translucent_half_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_translucent_half.vengpack";

    const Result<ArchiveReader> reader = CookMaterialPack(packJson, outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xC96});
    REQUIRE(entry.has_value());

    CookedMaterialHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedMaterialVersion);
    CHECK(header.Domain == 3u); // Translucent
    CHECK(header.HalfResolution == 1u);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: 'resolution: half' outside the Translucent domain is a located cook error")
{
    // Only the Translucent domain has a reduced-resolution layer to opt into; a Surface
    // material asking for it is rejected at cook time rather than silently ignored.
    const path packJson = FixtureDir / "material_surface_half_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_surface_half.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("requires the Translucent domain") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: an unknown cull mode is a located cook error")
{
    // Cull modes are serialized by enumerator name ("Back"/"Front"/"None"); a
    // lowercase or unknown value is rejected at cook time, not silently defaulted.
    const path packJson = FixtureDir / "material_bad_cull_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material_bad_cull.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("unknown cull mode") != string::npos);
    CHECK(result.error().find("none") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a translucent material whose fragment writes the MRT is a located cook error")
{
    // A Translucent material must write a single float4 SV_Target0 (HDR color + alpha), not the
    // g-buffer MRT. Pointing it at a g-buffer fragment shader violates the single-target contract.
    const path packJson = FixtureDir / "material_translucent_wrong_output_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_translucent_wrong.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("translucent material must write a single float4 SV_Target0") !=
          string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: an unknown domain is a located cook error")
{
    const path packJson = FixtureDir / "material_bad_domain_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_bad_domain.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("unknown domain") != string::npos);
    CHECK(result.error().find("translucent") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE(
    "Cooker: a surface material whose fragment shader writes one target is a located cook error")
{
    // A surface material must write the g-buffer MRT (SV_Target0..SV_Target4).
    // Pointing it at a shader that writes a single target is a contract mismatch.
    const path packJson = FixtureDir / "material_surface_wrong_output_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_surface_wrong.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("surface material must write the g-buffer") != string::npos);

    std::filesystem::remove(outArchive);
}

TEST_CASE(
    "Cooker: a postprocess material whose fragment shader writes the MRT is a located cook error")
{
    // A postprocess material must write a single float4 SV_Target0. Pointing it at
    // a g-buffer (MRT) fragment shader is a contract mismatch.
    const path packJson = FixtureDir / "material_postprocess_wrong_output_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_material_postprocess_wrong.vengpack";

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
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_material_oversize.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult result = cooker.CookPack(packJson, outArchive);

    REQUIRE(!result.has_value());
    CHECK(result.error().find("exceeds stride") != string::npos);

    std::filesystem::remove(outArchive);
}
