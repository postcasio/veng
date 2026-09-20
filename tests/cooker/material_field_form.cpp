// Material field-entry form test: a *.vmat.json "fields" entry states only what the cooker
// cannot reflect from the fragment shader, so the same material is authorable in two spellings.
// The cases here pin the pair and the four checks that keep the thinned spelling safe:
//   - a bare member-name string cooks to the same blob as the verbose entry it replaces;
//   - a "type" contradicting the reflected member is a located error naming both;
//   - a bare-string entry on a scalar-uint member is a located error (reflection cannot tell a
//     handle from a plain uint, so that member must state which it is);
//   - a "value" array of the wrong length is a located error naming both lengths;
//   - an instance override cooks the same over a thinned parent as over a verbose one.
//
// Each case writes a self-contained pack to a temp dir, so the fixtures read beside the assertion.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void WriteFile(const path& p, std::string_view contents)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    // A canonical-layout Surface vertex stage, so the pack's shaders both compile standalone.
    constexpr std::string_view SurfaceVert = R"(#include "Veng/surface.slang"
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1;
};
[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = output.sv_position;
    output.v_PrevClip = output.sv_position;
    output.v_UV = input.a_UV;
    output.v_WorldNormal = draw.NormalColumn0.xyz;
    output.v_WorldTangent = float4(draw.NormalColumn0.xyz, 1.0);
    output.v_MaterialIndex = draw.MaterialIndex;
    return output;
}
)";

    // A vec4, a float and a scalar uint — the three member shapes the entry forms distinguish.
    constexpr std::string_view TintFrag = R"(#include "Veng/surface.slang"
struct MaterialParams
{
    float4 Tint;
    float  Strength;
    uint   Mode;
};
[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams params = g_MaterialParams.Load<MaterialParams>(
        input.v_MaterialIndex * MaterialParamStride);
    GBufferOutput output;
    output.Albedo = params.Tint * params.Strength * float(params.Mode);
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = float2(0.0, 0.0);
    output.Emissive = float3(0.0);
    return output;
}
)";

    // Writes the shared shader pair into a fresh temp dir and returns it.
    path WriteShaderPack(const string& name)
    {
        const path dir = Veng::TestSupport::TempDir() / fmt::format("veng_matform_{}", name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);

        WriteFile(dir / "canonical.vlayout.json", R"({
  "elements": [
    { "format": "RGB32Sfloat",  "name": "a_Position" },
    { "format": "RGB32Sfloat",  "name": "a_Normal" },
    { "format": "RGBA32Sfloat", "name": "a_Tangent" },
    { "format": "RG32Sfloat",   "name": "a_UV" }
  ]
})");
        WriteFile(dir / "surface.vert.slang", SurfaceVert);
        WriteFile(
            dir / "surface.vert.shader.json",
            R"({ "source": "surface.vert.slang", "entry": "vsMain", "vertex_layout": "0x0000000000001B59" })");
        WriteFile(dir / "tint.frag.slang", TintFrag);
        WriteFile(dir / "tint.frag.shader.json",
                  R"({ "source": "tint.frag.slang", "entry": "fsMain", "domain": "Surface" })");
        return dir;
    }

    // Cooks a pack holding the shader pair plus the assets in @p extraAssets (a JSON fragment of
    // trailing "assets" entries), returning the reader or the located cook error.
    Result<ArchiveReader> CookPack(const path& dir, std::string_view extraAssets)
    {
        WriteFile(dir / "pack.json", fmt::format(R"({{
  "version": 1,
  "assets": [
    {{ "id": "0x0000000000001B59", "type": "VertexLayout", "source": "canonical.vlayout.json" }},
    {{ "id": "0x0000000000001B5A", "type": "Shader",       "source": "surface.vert.shader.json" }},
    {{ "id": "0x0000000000001B5B", "type": "Shader",       "source": "tint.frag.shader.json" }}{}
  ]
}})",
                                                 extraAssets));

        const path outArchive = dir / "out.vengpack";
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const VoidResult cooked =
            cooker.CookPack(dir / "pack.json", outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                            {}, path(VENG_CORE_SHADER_DIR));
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        return ArchiveReader::Open(outArchive);
    }

    // Writes a material naming the three members with the given "fields" body.
    void WriteMaterial(const path& dir, std::string_view file, std::string_view fields)
    {
        WriteFile(dir / file, fmt::format(R"({{
  "domain": "Surface",
  "shaders": {{ "vertex": "0x0000000000001B5A", "fragment": "0x0000000000001B5B" }},
  "fields": [{}]
}})",
                                          fields));
    }

    constexpr std::string_view OneMaterial =
        R"(,
    { "id": "0x0000000000001B5D", "type": "Material", "source": "thin.vmat.json" })";
}

TEST_CASE("Cooker: a thinned field list cooks to the same material blob as the verbose one")
{
    const path dir = WriteShaderPack("equal");

    // Tint takes the zero default its verbose twin spells out; Strength and Mode state only
    // their values and, for the scalar uint, the kind reflection cannot supply.
    WriteMaterial(dir, "thin.vmat.json", R"(
    "Tint",
    { "name": "Strength", "value": 2.5 },
    { "name": "Mode", "type": "uint", "value": 3 })");
    WriteMaterial(dir, "verbose.vmat.json", R"(
    { "name": "Tint", "type": "vec4", "value": [0.0, 0.0, 0.0, 0.0] },
    { "name": "Strength", "type": "float", "value": 2.5 },
    { "name": "Mode", "type": "uint", "value": 3 })");

    const Result<ArchiveReader> reader = CookPack(dir, R"(,
    { "id": "0x0000000000001B5D", "type": "Material", "source": "thin.vmat.json" },
    { "id": "0x0000000000001B5F", "type": "Material", "source": "verbose.vmat.json" })");
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    const optional<ArchiveEntry> thin = reader->Find(AssetId{0x0000000000001B5D});
    const optional<ArchiveEntry> verbose = reader->Find(AssetId{0x0000000000001B5F});
    REQUIRE(thin.has_value());
    REQUIRE(verbose.has_value());

    // Same field table, same block image, same header — the two sources say the same thing.
    REQUIRE(thin->Blob.size() == verbose->Blob.size());
    CHECK(std::memcmp(thin->Blob.data(), verbose->Blob.data(), thin->Blob.size()) == 0);

    // And the bare name really did take the zero default rather than leaving the member out.
    CookedMaterialHeader header{};
    std::memcpy(&header, thin->Blob.data(), sizeof(header));
    CHECK(header.FieldCount == 3);
    const u8* block = thin->Blob.data() + sizeof(CookedMaterialHeader) +
                      header.FieldCount * sizeof(CookedMaterialField);
    f32 tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::memcpy(tint, block, sizeof(tint));
    CHECK(tint[0] == 0.0f);
    CHECK(tint[1] == 0.0f);
    CHECK(tint[2] == 0.0f);
    CHECK(tint[3] == 0.0f);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: a field 'type' contradicting the reflected member is a located cook error")
{
    const path dir = WriteShaderPack("mismatch");
    WriteMaterial(dir, "thin.vmat.json", R"(
    "Tint",
    { "name": "Strength", "type": "vec4", "value": [1.0, 1.0, 1.0, 1.0] },
    { "name": "Mode", "type": "uint" })");

    const Result<ArchiveReader> reader = CookPack(dir, OneMaterial);
    REQUIRE(!reader.has_value());
    // The error names the member and both types — the declared one and the reflected one.
    CHECK(reader.error().find("Strength") != string::npos);
    CHECK(reader.error().find("'vec4'") != string::npos);
    CHECK(reader.error().find("'float'") != string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: a bare-name entry on a scalar-uint member is a located cook error")
{
    const path dir = WriteShaderPack("bareuint");
    WriteMaterial(dir, "thin.vmat.json", R"(
    "Tint",
    { "name": "Strength", "value": 2.5 },
    "Mode")");

    // A texture, sampler, volume, storage buffer and plain uint param are all the same bare uint
    // to reflection, so a bare name over one would cook a handle as a zero-valued param and the
    // shader would sample bindless slot 0 — a wrong texture, not a fault.
    const Result<ArchiveReader> reader = CookPack(dir, OneMaterial);
    REQUIRE(!reader.has_value());
    CHECK(reader.error().find("Mode") != string::npos);
    CHECK(reader.error().find("scalar uint") != string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: a 'value' array of the wrong length is a located cook error")
{
    const path dir = WriteShaderPack("arity");
    WriteMaterial(dir, "thin.vmat.json", R"(
    { "name": "Tint", "value": [1.0, 0.5, 0.25] },
    { "name": "Strength", "value": 2.5 },
    { "name": "Mode", "type": "uint" })");

    // With the type gone from the entry, reflection is what bounds the write — without this
    // check a longer array would run past the member and overwrite the one after it.
    const Result<ArchiveReader> reader = CookPack(dir, OneMaterial);
    REQUIRE(!reader.has_value());
    CHECK(reader.error().find("Tint") != string::npos);
    CHECK(reader.error().find("3 elements") != string::npos);
    CHECK(reader.error().find("4 components") != string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: an instance override cooks the same over a thinned parent as a verbose one")
{
    const path dir = WriteShaderPack("instance");
    WriteMaterial(dir, "thin.vmat.json", R"(
    "Tint",
    { "name": "Strength", "value": 2.5 },
    { "name": "Mode", "type": "uint", "value": 3 })");
    WriteMaterial(dir, "verbose.vmat.json", R"(
    { "name": "Tint", "type": "vec4", "value": [0.0, 0.0, 0.0, 0.0] },
    { "name": "Strength", "type": "float", "value": 2.5 },
    { "name": "Mode", "type": "uint", "value": 3 })");
    WriteFile(dir / "over_thin.vmatinst.json",
              R"({ "parent": "0x0000000000001B5D",
                   "overrides": { "Tint": [0.25, 0.5, 0.75, 1.0], "Strength": 4.0 } })");
    WriteFile(dir / "over_verbose.vmatinst.json",
              R"({ "parent": "0x0000000000001B5F",
                   "overrides": { "Tint": [0.25, 0.5, 0.75, 1.0], "Strength": 4.0 } })");

    const Result<ArchiveReader> reader = CookPack(dir, R"(,
    { "id": "0x0000000000001B5D", "type": "Material",         "source": "thin.vmat.json" },
    { "id": "0x0000000000001B5F", "type": "Material",         "source": "verbose.vmat.json" },
    { "id": "0x0000000000001B60", "type": "MaterialInstance", "source": "over_thin.vmatinst.json" },
    { "id": "0x0000000000001B61", "type": "MaterialInstance", "source": "over_verbose.vmatinst.json" })");
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    const optional<ArchiveEntry> overThin = reader->Find(AssetId{0x0000000000001B60});
    const optional<ArchiveEntry> overVerbose = reader->Find(AssetId{0x0000000000001B61});
    REQUIRE(overThin.has_value());
    REQUIRE(overVerbose.has_value());

    // The parent ids differ by construction; everything the parent's field list decides — the
    // override table and the packed value region — must not.
    REQUIRE(overThin->Blob.size() == overVerbose->Blob.size());
    constexpr usize AfterParentId = sizeof(u64);
    CHECK(std::memcmp(overThin->Blob.data() + AfterParentId,
                      overVerbose->Blob.data() + AfterParentId,
                      overThin->Blob.size() - AfterParentId) == 0);

    CookedMaterialInstanceHeader header{};
    std::memcpy(&header, overThin->Blob.data(), sizeof(header));
    CHECK(header.OverrideCount == 2);

    std::filesystem::remove_all(dir);
}
