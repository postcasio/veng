// Shader cook test: cooks the fixture shader_pack.json through libveng_cook
// and checks the resulting CookedShaderHeader + CookedShaderInterfaceHeader +
// reflected tables (bindings, push constants):
//   - vertex stage (entry 4001, mesh.vert.slang via mesh.vert.shader.json)
//   - fragment stage (entry 4002, brick.frag.slang via brick.frag.shader.json)
//   - compute stage (grid.comp.slang via grid.comp.shader.json), whose [numthreads] the cooked
//     interface carries
//
// The blob is read back through ParseCookedShader, the loader's own parser.
// Shaders reference their vertex layout by AssetId
// (CookedShaderInterfaceHeader::VertexLayoutAssetId) rather than embedding a
// per-shader input table.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"
#include <random>
#include <string_view>

#include "FixturePack.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/CookedShader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ShaderTargets.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    Result<ArchiveReader> CookShaderPack(path& outArchive)
    {
        const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
        const path packJson = fixtureDir / "shader_pack.json";
        // Unique per call: ctest runs each case as its own process in parallel, and a
        // shared fixed name lets concurrent cases cook over and delete each other's archive.
        std::random_device rng;
        outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_cooker_shader_{:08x}.vengpack", rng());

        // The cases read the SPIR-V section, which a Metal tree's default targets leave out.
        Cooker cooker;
        cooker.SetShaderTargets(ShaderTargetSet{}.With(ShaderTarget::SpirV));
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = Veng::Test::CookFixturePack(cooker, packJson, outArchive);
        REQUIRE(cookResult.has_value());

        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: cooks a shader from .slang source via Slang reflection")
{
    path outArchive;
    const Result<ArchiveReader> reader = CookShaderPack(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xFA1});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Shader);

    const Result<CookedShaderView> shader = ParseCookedShader(entry->Blob);
    REQUIRE_MESSAGE(shader.has_value(), shader.error());

    // Slang always emits "main" for the SPIR-V OpEntryPoint name, regardless of the source
    // entry-point name.
    const Result<const CookedShaderSectionView*> spirv =
        FindCookedShaderSection(*shader, CookedShaderCode::SpirV, "mesh.vert");
    REQUIRE(spirv.has_value());
    CHECK((*spirv)->EntryPoint == "main");
    CHECK(!(*spirv)->Bytes.empty());

    // mesh.vert.slang: no descriptor bindings (its only resources are the bindless registry's,
    // excluded), one push-constant block (PushConstants: float4x4 MVP + float4x4 Model = 128
    // bytes). Vertex layout is referenced by AssetId 7001 (canonical.vlayout.json).
    CHECK(shader->Bindings.empty());
    REQUIRE(shader->PushConstants.size() == 1);
    CHECK(shader->Interface.VertexLayoutAssetId == 7001ULL);

    // Push-constant block: g_PushConstants, 128 bytes, vertex stage (mask 1).
    const CookedPushConstantBlock& pushConstant = shader->PushConstants[0];
    CHECK(pushConstant.Offset == 0);
    CHECK(pushConstant.Size == 128);
    CHECK(pushConstant.StageMask == 1u); // ShaderStage::Vertex underlying value
    CHECK(std::string_view(pushConstant.Name) == "g_PushConstants");

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: cooks a fragment shader from .slang source via Slang reflection")
{
    path outArchive;
    const Result<ArchiveReader> reader = CookShaderPack(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xFA2});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Shader);

    const Result<CookedShaderView> shader = ParseCookedShader(entry->Blob);
    REQUIRE_MESSAGE(shader.has_value(), shader.error());

    const Result<const CookedShaderSectionView*> spirv =
        FindCookedShaderSection(*shader, CookedShaderCode::SpirV, "brick.frag");
    REQUIRE(spirv.has_value());
    CHECK((*spirv)->EntryPoint == "main");
    CHECK(!(*spirv)->Bytes.empty());

    // brick.frag.slang: all resources live in the bindless registry's sets (excluded from the
    // declared interface), so no descriptor bindings; one push-constant block; no vertex layout.
    CHECK(shader->Bindings.empty());
    REQUIRE(shader->PushConstants.size() == 1);
    CHECK(shader->Interface.VertexLayoutAssetId == 0ULL);

    const CookedPushConstantBlock& pushConstant = shader->PushConstants[0];
    CHECK(pushConstant.Offset == 0);
    CHECK(pushConstant.StageMask == 2u); // ShaderStage::Fragment underlying value
    CHECK(pushConstant.Size > 0);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a compute shader's threadgroup size is cooked axis by axis")
{
    path outArchive;
    const Result<ArchiveReader> reader = CookShaderPack(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x5C9F7BDAE2311FD0ULL});
    REQUIRE(entry.has_value());
    const Result<CookedShaderView> shader = ParseCookedShader(entry->Blob);
    REQUIRE_MESSAGE(shader.has_value(), shader.error());

    // grid.comp.slang declares [numthreads(8, 4, 2)].
    CHECK(shader->Interface.ThreadgroupSize[0] == 8u);
    CHECK(shader->Interface.ThreadgroupSize[1] == 4u);
    CHECK(shader->Interface.ThreadgroupSize[2] == 2u);

    // A stage other than compute has none.
    const optional<ArchiveEntry> fragment = reader->Find(AssetId{0xFA2});
    REQUIRE(fragment.has_value());
    const Result<CookedShaderView> fragmentShader = ParseCookedShader(fragment->Blob);
    REQUIRE(fragmentShader.has_value());
    CHECK(fragmentShader->Interface.ThreadgroupSize[0] == 0u);

    std::filesystem::remove(outArchive);
}
