// UI-document cook test: cooks a fixture pack with a Texture and a UIDocument whose <Image> names
// that texture as its `src`. Asserts the cook succeeds (an `<Image src=…>` is a recognized
// attribute, not an unrecognized-attribute error), that the cooked recipe's Image element carries
// the source texture id + tint + UV, and that the referenced texture is a real archive entry the
// runtime loader eager-loads as a dependency.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId TextureId{0x4DE87D318251BC1EULL};
    constexpr AssetId UIDocumentId{0x2D5E729A12F26978ULL};
}

TEST_CASE("Cooker: an <Image src=…> cooks and records the texture on the recipe element")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_image_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_ui_image.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // The cook succeeds — an `src` on an <Image> is recognized, not an unrecognized-attribute error.
    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    // The source texture is a real archive entry (the runtime loader eager-loads it as a dependency).
    const optional<ArchiveEntry> textureEntry = reader->Find(TextureId);
    REQUIRE(textureEntry.has_value());
    CHECK(textureEntry->Type == AssetType::Texture);

    const optional<ArchiveEntry> docEntry = reader->Find(UIDocumentId);
    REQUIRE(docEntry.has_value());
    CHECK(docEntry->Type == AssetType::UIDocument);

    const std::span<const u8> blob = docEntry->Blob;
    REQUIRE(blob.size() >= sizeof(CookedUIDocumentHeader));

    CookedUIDocumentHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.Version == CookedUIDocumentVersion);
    // The recipe tree is a Panel root with one Image child.
    REQUIRE(header.ElementCount == 2);

    // The element array follows the header + the stylesheet-id list.
    const usize elementsOffset =
        sizeof(CookedUIDocumentHeader) + static_cast<usize>(header.StyleSheetCount) * sizeof(u64);
    REQUIRE(blob.size() >= elementsOffset + 2 * sizeof(CookedUIElement));

    CookedUIElement panel{};
    std::memcpy(&panel, blob.data() + elementsOffset, sizeof(panel));
    CHECK(panel.Kind == static_cast<u32>(Gui::ElementKind::Panel));
    // The Panel itself sources no texture.
    CHECK(panel.Src == 0);

    CookedUIElement image{};
    std::memcpy(&image, blob.data() + elementsOffset + sizeof(CookedUIElement), sizeof(image));
    CHECK(image.Kind == static_cast<u32>(Gui::ElementKind::Image));

    // The Image's `src` recorded the texture id, and its tint / UV took the defaults (no tint/uv
    // attribute authored): opaque white, the whole texture.
    CHECK(image.Src == TextureId.Value);
    CHECK(image.Tint[0] == doctest::Approx(1.0f));
    CHECK(image.Tint[1] == doctest::Approx(1.0f));
    CHECK(image.Tint[2] == doctest::Approx(1.0f));
    CHECK(image.Tint[3] == doctest::Approx(1.0f));
    CHECK(image.Uv[0] == doctest::Approx(0.0f));
    CHECK(image.Uv[1] == doctest::Approx(0.0f));
    CHECK(image.Uv[2] == doctest::Approx(1.0f));
    CHECK(image.Uv[3] == doctest::Approx(1.0f));

    std::filesystem::remove(outArchive);
}
