// Shader cook test: cooks the fixture shader_pack.json through libveng_cook
// and checks the resulting CookedShaderHeader + CookedShaderInterfaceHeader +
// reflected tables (bindings, push constants):
//   - vertex stage (entry 4001, mesh.vert.slang via mesh.vert.shader.json)
//   - fragment stage (entry 4002, brick.frag.slang via brick.frag.shader.json)
//
// Blob layout: header → bindings → push constants → SPIR-V.
// Shaders reference their vertex layout by AssetId
// (CookedShaderInterfaceHeader::VertexLayoutAssetId) rather than embedding a
// per-shader input table.

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
    Result<ArchiveReader> CookShaderPack()
    {
        const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
        const path packJson = fixtureDir / "shader_pack.json";
        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_cooker_shader.vengpack";

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
        REQUIRE(cookResult.has_value());

        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: cooks a shader from .slang source via Slang reflection")
{
    const Result<ArchiveReader> reader = CookShaderPack();
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xFA1});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Shader);

    REQUIRE(entry->Blob.size() >= sizeof(CookedShaderHeader));

    CookedShaderHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    // Slang always emits "main" for the SPIR-V OpEntryPoint name, regardless
    // of the source entry-point name (confirmed via the importer's comment).
    CHECK(std::string_view(header.EntryPoint) == "main");
    CHECK(header.SpirvBytes > 0);

    usize cursor = sizeof(CookedShaderHeader);
    REQUIRE(entry->Blob.size() >= cursor + sizeof(CookedShaderInterfaceHeader));

    CookedShaderInterfaceHeader interfaceHeader{};
    std::memcpy(&interfaceHeader, entry->Blob.data() + cursor, sizeof(interfaceHeader));
    cursor += sizeof(CookedShaderInterfaceHeader);

    // mesh.vert.slang: no descriptor bindings (no set >= 1 resources —
    // set 0 is the bindless registry and is excluded), one push-constant
    // block (PushConstants: float4x4 MVP + float4x4 Model = 128 bytes).
    // Vertex layout is referenced by AssetId 7001 (canonical.vlayout.json).
    CHECK(interfaceHeader.BindingCount == 0);
    CHECK(interfaceHeader.PushConstantCount == 1);
    CHECK(interfaceHeader.VertexLayoutAssetId == 7001ULL);

    cursor += interfaceHeader.BindingCount * sizeof(CookedDescriptorBinding);

    // Push-constant block: g_PushConstants, 128 bytes, vertex stage (mask 1).
    REQUIRE(entry->Blob.size() >= cursor + sizeof(CookedPushConstantBlock));
    CookedPushConstantBlock pushConstant{};
    std::memcpy(&pushConstant, entry->Blob.data() + cursor, sizeof(pushConstant));
    CHECK(pushConstant.Offset == 0);
    CHECK(pushConstant.Size == 128);
    CHECK(pushConstant.StageMask == 1u); // ShaderStage::Vertex underlying value
    CHECK(std::string_view(pushConstant.Name) == "g_PushConstants");

    std::filesystem::remove(std::filesystem::temp_directory_path() / "veng_cooker_shader.vengpack");
}

TEST_CASE("Cooker: cooks a fragment shader from .slang source via Slang reflection")
{
    const Result<ArchiveReader> reader = CookShaderPack();
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xFA2});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Shader);

    REQUIRE(entry->Blob.size() >= sizeof(CookedShaderHeader));

    CookedShaderHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    // Slang always emits "main" for the SPIR-V OpEntryPoint name.
    CHECK(std::string_view(header.EntryPoint) == "main");
    CHECK(header.SpirvBytes > 0);

    usize cursor = sizeof(CookedShaderHeader);
    REQUIRE(entry->Blob.size() >= cursor + sizeof(CookedShaderInterfaceHeader));

    CookedShaderInterfaceHeader interfaceHeader{};
    std::memcpy(&interfaceHeader, entry->Blob.data() + cursor, sizeof(interfaceHeader));
    cursor += sizeof(CookedShaderInterfaceHeader);

    // brick.frag.slang: all resources live in set 0 (the bindless registry,
    // excluded from the declared interface), so no descriptor bindings; one
    // push-constant block (PushConstants); no vertex layout (fragment stage,
    // vertex_layout omitted → VertexLayoutAssetId 0).
    CHECK(interfaceHeader.BindingCount == 0);
    CHECK(interfaceHeader.PushConstantCount == 1);
    CHECK(interfaceHeader.VertexLayoutAssetId == 0ULL);

    cursor += interfaceHeader.BindingCount * sizeof(CookedDescriptorBinding);

    REQUIRE(entry->Blob.size() >= cursor + sizeof(CookedPushConstantBlock));
    CookedPushConstantBlock pushConstant{};
    std::memcpy(&pushConstant, entry->Blob.data() + cursor, sizeof(pushConstant));
    CHECK(pushConstant.Offset == 0);
    CHECK(pushConstant.StageMask == 2u); // ShaderStage::Fragment underlying value
    CHECK(pushConstant.Size > 0);

    std::filesystem::remove(std::filesystem::temp_directory_path() / "veng_cooker_shader.vengpack");
}
