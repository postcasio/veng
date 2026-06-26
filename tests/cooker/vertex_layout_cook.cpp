// Vertex-layout cook test: cooks packs containing
// vertex_layout assets and verifies the resulting blobs round-trip correctly.
// Also tests that cooking a shader whose reflected vertex inputs don't match
// the referenced layout fails with a diagnostic.

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

TEST_CASE("Cooker: cooks a vertex_layout asset and produces correct blob")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "shader_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_vlayout.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    // The shader_pack.json has vertex_layout asset at id 7001.
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x1B59});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::VertexLayout);

    REQUIRE(entry->Blob.size() >= sizeof(CookedVertexLayoutHeader));

    CookedVertexLayoutHeader layoutHeader{};
    std::memcpy(&layoutHeader, entry->Blob.data(), sizeof(layoutHeader));
    CHECK(layoutHeader.ElementCount == 4);

    REQUIRE(entry->Blob.size() >=
            sizeof(CookedVertexLayoutHeader) +
                layoutHeader.ElementCount * sizeof(CookedVertexLayoutElement));

    // Formats: RGB32Sfloat=9, RG32Sfloat=8 (Renderer::Format underlying values).
    const u32 expectedFormats[] = {9, 9, 9, 8};
    const std::string_view expectedNames[] = {"a_Position", "a_Normal", "a_Tangent", "a_UV"};

    for (u32 i = 0; i < layoutHeader.ElementCount; ++i)
    {
        CookedVertexLayoutElement elem{};
        std::memcpy(&elem,
                    entry->Blob.data() + sizeof(CookedVertexLayoutHeader) +
                        i * sizeof(CookedVertexLayoutElement),
                    sizeof(elem));
        CHECK(elem.Format == expectedFormats[i]);
        CHECK(std::string_view(elem.Name) == expectedNames[i]);
    }

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: cooking a shader with mismatched vertex inputs fails")
{
    // shader_mismatch_pack.json: a 1-element layout (positiononly) paired with
    // mesh.vert.slang which reflects 4 inputs. The cooker must reject this.
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "shader_mismatch_pack.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_mismatch.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult r = cooker.CookPack(packJson, outArchive);
    CHECK(!r.has_value());
    // The error should mention the mismatch between reflected inputs and layout.
    if (!r.has_value())
    {
        CHECK(r.error().find("mismatch") != std::string::npos);
    }

    // Clean up if the file was (incorrectly) written.
    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a shader resolves a vertex layout declared in a sibling pack")
{
    // crosspack_shader.json holds only the shader (mesh.vert, referencing layout id 7001);
    // crosspack_layout.json holds only that layout. Passing the layout pack as a reference
    // resolves the cross-pack AssetId at cook time — the same path a project's packs use to
    // reference each other (and the engine core pack). Without the reference the cook fails.
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path shaderPack = fixtureDir / "crosspack_shader.json";
    const path layoutPack = fixtureDir / "crosspack_layout.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_crosspack.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // Without the sibling reference the layout id is unresolvable and the shader cook fails.
    CHECK_FALSE(cooker.CookPack(shaderPack, outArchive).has_value());

    // With the sibling pack referenced, the cross-pack layout resolves and the shader cooks.
    const path refs[] = {layoutPack};
    const VoidResult cooked = cooker.CookPack(shaderPack, outArchive, refs);
    REQUIRE(cooked.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> shader = reader->Find(AssetId{0x0FA1}); // 4001
    REQUIRE(shader.has_value());
    CHECK(shader->Type == AssetType::Shader);

    std::filesystem::remove(outArchive);
}
