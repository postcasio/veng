// Material-instance cook test: cooks a *.vmatinst.json over a parent Material and
// checks the resulting CookedMaterialInstanceHeader + override table + value region.
// It mirrors the .vmat-against-shader validation, lifted to instance-against-parent:
//   - a valid instance overriding one exposed vec4 param + one exposed texture cooks
//     to a blob whose overrides reference the parent fields by name;
//   - an override naming a field the parent does not expose is a located cook error;
//   - an override naming an engine-bound field (present in the shader but not the
//     parent's declared exposed list) is a located cook error;
//   - a type mismatch (a scalar override of a vec4 field) is a located cook error.
//
// The parent is a hand-authored Surface fragment with a MaterialParams the cooker
// reflects, so the test is self-contained and does not depend on the graph walk.

#include <cstring>
#include <filesystem>
#include <fstream>
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
    void WriteFile(const path& p, std::string_view contents)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    // A canonical-layout Surface vertex stage (matches the core surface.vert contract),
    // so the parent material's shaders both compile from a self-contained pack.
    constexpr std::string_view SurfaceVert = R"(#include "Veng/material.slang"
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
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));
    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = input.a_UV;
    output.v_WorldNormal = draw.NormalColumn0.xyz;
    output.v_WorldTangent = float4(draw.NormalColumn0.xyz, 1.0);
    output.v_MaterialIndex = draw.MaterialIndex;
    return output;
}
)";

    // A parent fragment exposing BaseColor (texture+sampler) + BaseColorFactor (vec4),
    // plus an engine-bound-only field that the parent .vmat does NOT declare exposed,
    // so an instance overriding it is rejected as non-exposed.
    constexpr std::string_view BrickFrag = R"(#include "Veng/material.slang"
struct MaterialParams
{
    float4 BaseColorFactor;
    uint   BaseColor;
    uint   BaseColorSampler;
    uint   EngineBound;
};
[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams params = g_MaterialParams.Load<MaterialParams>(
        input.v_MaterialIndex * MaterialParamStride);
    GBufferOutput output;
    output.Albedo = params.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    return output;
}
)";

    // Writes the parent pack (vertex + fragment shaders, a texture, the parent material)
    // and returns its dir. The instance source is written by each test case.
    path WriteParentPack(const string& name)
    {
        const path dir =
            std::filesystem::temp_directory_path() / fmt::format("veng_matinst_cook_{}", name);
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
            R"({ "source": "surface.vert.slang", "entry": "vsMain", "vertex_layout": 7001 })");

        WriteFile(dir / "brick.frag.slang", BrickFrag);
        WriteFile(dir / "brick.frag.shader.json",
                  R"({ "source": "brick.frag.slang", "entry": "fsMain", "domain": "surface" })");

        // A 1x1 white texture so the parent's BaseColor handle resolves.
        WriteFile(dir / "white.tex.json",
                  R"({ "image": "white.png", "srgb": true, "compression": "none",
                       "generate_mips": false })");
        // Minimal 1x1 white PNG.
        const unsigned char png[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
            0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00,
            0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08,
            0xD7, 0x63, 0xF8, 0xFF, 0xFF, 0x3F, 0x00, 0x05, 0xFE, 0x02, 0xFE, 0xDC, 0xCC, 0x59,
            0xE7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
        std::ofstream pngOut(dir / "white.png", std::ios::binary | std::ios::trunc);
        pngOut.write(reinterpret_cast<const char*>(png), sizeof(png));
        pngOut.close();

        // The parent exposes BaseColorFactor (vec4) + BaseColor (texture) + BaseColorSampler;
        // EngineBound is reflected in the shader but NOT declared here, so it is not exposed.
        WriteFile(dir / "brick.vmat.json", R"({
  "domain": "surface",
  "shaders": { "vertex": 7002, "fragment": 7003 },
  "fields": [
    { "name": "BaseColorFactor",  "type": "vec4",    "value": [1.0, 1.0, 1.0, 1.0] },
    { "name": "BaseColor",        "type": "texture", "id": 7004 },
    { "name": "BaseColorSampler", "type": "sampler", "texture": "BaseColor" }
  ]
})");

        return dir;
    }

    // Cooks a pack containing the parent + the named instance source (id 7006), returning the
    // reader on success or the located cook error on failure.
    Result<ArchiveReader> CookWithInstance(const path& dir, std::string_view instanceSource)
    {
        WriteFile(dir / "pack.json", fmt::format(R"({{
  "version": 1,
  "assets": [
    {{ "id": 7001, "type": "vertex_layout",     "source": "canonical.vlayout.json" }},
    {{ "id": 7002, "type": "shader",            "source": "surface.vert.shader.json" }},
    {{ "id": 7003, "type": "shader",            "source": "brick.frag.shader.json" }},
    {{ "id": 7004, "type": "texture",           "source": "white.tex.json" }},
    {{ "id": 7005, "type": "material",          "source": "brick.vmat.json" }},
    {{ "id": 7006, "type": "material_instance", "source": "{}" }}
  ]
}})",
                                                 instanceSource));

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
}

TEST_CASE("Cooker: a material instance overriding an exposed vec4 + texture cooks to a valid blob")
{
    const path dir = WriteParentPack("valid");
    WriteFile(dir / "tinted.vmatinst.json", R"({
  "parent": 7005,
  "overrides": {
    "BaseColorFactor": [0.9, 0.4, 0.2, 1.0],
    "BaseColor": 7004
  }
})");

    const Result<ArchiveReader> reader = CookWithInstance(dir, "tinted.vmatinst.json");
    REQUIRE_MESSAGE(reader.has_value(), reader.error());

    const optional<ArchiveEntry> inst = reader->Find(AssetId{7006});
    REQUIRE(inst.has_value());
    CHECK(inst->Type == AssetType::MaterialInstance);
    REQUIRE(inst->Blob.size() >= sizeof(CookedMaterialInstanceHeader));

    CookedMaterialInstanceHeader header{};
    std::memcpy(&header, inst->Blob.data(), sizeof(header));
    CHECK(header.ParentId == 7005ULL);
    CHECK(header.Version == CookedMaterialInstanceVersion);
    CHECK(header.OverrideCount == 2);
    CHECK(header.ValueRegionBytes == 16); // one vec4 param value (the texture carries no bytes)

    const auto* overrides = reinterpret_cast<const CookedMaterialInstanceOverride*>(
        inst->Blob.data() + sizeof(CookedMaterialInstanceHeader));

    // Find the param + texture overrides by name (order follows the JSON object iteration).
    const CookedMaterialInstanceOverride* paramOv = nullptr;
    const CookedMaterialInstanceOverride* texOv = nullptr;
    for (u32 i = 0; i < header.OverrideCount; ++i)
    {
        if (std::string_view(overrides[i].Name) == "BaseColorFactor")
        {
            paramOv = &overrides[i];
        }
        else if (std::string_view(overrides[i].Name) == "BaseColor")
        {
            texOv = &overrides[i];
        }
    }
    REQUIRE(paramOv != nullptr);
    REQUIRE(texOv != nullptr);

    CHECK(paramOv->Kind == 0u);
    CHECK(paramOv->ValueSize == 16u);
    CHECK(texOv->Kind == 1u);
    CHECK(texOv->TextureId == 7004ULL);
    CHECK(texOv->ValueSize == 0u);

    // The override value bytes are packed into the value region at the param's ValueOffset.
    const u8* valueRegion = inst->Blob.data() + sizeof(CookedMaterialInstanceHeader) +
                            header.OverrideCount * sizeof(CookedMaterialInstanceOverride);
    f32 packed[4];
    std::memcpy(packed, valueRegion + paramOv->ValueOffset, sizeof(packed));
    CHECK(packed[0] == doctest::Approx(0.9f));
    CHECK(packed[1] == doctest::Approx(0.4f));
    CHECK(packed[2] == doctest::Approx(0.2f));
    CHECK(packed[3] == doctest::Approx(1.0f));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: an override naming a non-exposed field is a located cook error")
{
    const path dir = WriteParentPack("nonexposed");
    WriteFile(dir / "bad.vmatinst.json", R"({
  "parent": 7005,
  "overrides": { "NotAField": [1.0, 0.0, 0.0, 1.0] }
})");

    const Result<ArchiveReader> reader = CookWithInstance(dir, "bad.vmatinst.json");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("NotAField") != string::npos);
    CHECK(reader.error().find("not an exposed field") != string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: an override naming an engine-bound (non-exposed) field is a located cook error")
{
    // EngineBound is a MaterialParams member but is not in the parent's declared "fields",
    // so it is not an override surface — exactly the engine-bound exclusion.
    const path dir = WriteParentPack("enginebound");
    WriteFile(dir / "bad.vmatinst.json", R"({
  "parent": 7005,
  "overrides": { "EngineBound": 3 }
})");

    const Result<ArchiveReader> reader = CookWithInstance(dir, "bad.vmatinst.json");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("EngineBound") != string::npos);
    CHECK(reader.error().find("not an exposed field") != string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("Cooker: a type-mismatched override (scalar over a vec4 field) is a located cook error")
{
    const path dir = WriteParentPack("typemismatch");
    WriteFile(dir / "bad.vmatinst.json", R"({
  "parent": 7005,
  "overrides": { "BaseColorFactor": 0.5 }
})");

    const Result<ArchiveReader> reader = CookWithInstance(dir, "bad.vmatinst.json");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().find("BaseColorFactor") != string::npos);

    std::filesystem::remove_all(dir);
}
