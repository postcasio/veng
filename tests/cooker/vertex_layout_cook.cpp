// Vertex-layout cook test (planset-5 plan 08b): cooks packs containing
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
    const optional<ArchiveEntry> entry = reader->Find(AssetId{7001});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::VertexLayout);

    REQUIRE(entry->Blob.size() >= sizeof(CookedVertexLayoutHeader));

    CookedVertexLayoutHeader layoutHeader{};
    std::memcpy(&layoutHeader, entry->Blob.data(), sizeof(layoutHeader));
    CHECK(layoutHeader.ElementCount == 4);

    REQUIRE(entry->Blob.size() >= sizeof(CookedVertexLayoutHeader) +
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
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_mismatch.vengpack";

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
