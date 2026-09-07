// UI-document cook test for the Dropdown widget: a `<Dropdown>` with an `items`/`value` binding and
// an option item template cooks through the authoring surface (the tag parses and its config
// attributes are recognized), and an authored `<DropdownArrow>` — a widget-owned part kind kept out
// of the parser, exactly as `<ScrollBar>` is — is a located cook error rather than a silent element.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string_view>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId DocumentId{0x2D5E729A12F26978ULL};

    // The decoded element records of a cooked UI-document blob.
    struct DecodedDocument
    {
        CookedUIDocumentHeader Header{};
        vector<CookedUIElement> Elements;
    };

    // Cooks a single-UIDocument pack whose source is `markup`, returning the decoded elements or the
    // cook's located error.
    Result<DecodedDocument> CookMarkup(std::string_view markup)
    {
        std::random_device rng;
        const string stem = fmt::format("veng_ui_dropdown_{:08x}", rng());
        const path dir = Veng::TestSupport::TempDir();
        const path sourcePath = dir / (stem + ".vui.xml");
        const path packPath = dir / (stem + ".pack.json");
        const path outArchive = dir / (stem + ".vengpack");

        std::ofstream(sourcePath) << markup;

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = FormatHexId(DocumentId.Value);
        asset["type"] = "UIDocument";
        asset["source"] = sourcePath.filename().string();
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = cooker.CookPack(packPath, outArchive);
        if (!cookResult.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(reader.error());
        }
        const optional<ArchiveEntry> entry = reader->Find(DocumentId);
        if (!entry.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(string("document entry missing from archive"));
        }

        const std::span<const u8> blob = entry->Blob;
        DecodedDocument decoded;
        std::memcpy(&decoded.Header, blob.data(), sizeof(decoded.Header));
        const usize offset = sizeof(CookedUIDocumentHeader) +
                             static_cast<usize>(decoded.Header.StyleSheetCount) * sizeof(u64);
        decoded.Elements.resize(decoded.Header.ElementCount);
        std::memcpy(decoded.Elements.data(), blob.data() + offset,
                    decoded.Elements.size() * sizeof(CookedUIElement));

        std::filesystem::remove(outArchive);
        return decoded;
    }
}

TEST_CASE("ui dropdown cook: a <Dropdown> with an item template and config attributes cooks")
{
    const Result<DecodedDocument> doc = CookMarkup(
        R"(<Panel>
             <Dropdown items="{model.options}" value="{model.selected}">
               <Text text="{option.label}"/>
             </Dropdown>
           </Panel>)");
    REQUIRE_MESSAGE(doc.has_value(), "cook failed: ", doc ? string{} : doc.error());

    // Panel root, the Dropdown, and its one Text option template — three elements, in pre-order.
    REQUIRE(doc->Elements.size() == 3);
    CHECK(doc->Elements[1].Kind == static_cast<u32>(Gui::ElementKind::Dropdown));
    CHECK(doc->Elements[1].ChildCount == 1);
    CHECK(doc->Elements[2].Kind == static_cast<u32>(Gui::ElementKind::Text));
}

TEST_CASE("ui dropdown cook: an authored <DropdownArrow> is a located cook error")
{
    const Result<DecodedDocument> doc = CookMarkup(R"(<Dropdown><DropdownArrow/></Dropdown>)");
    REQUIRE_FALSE(doc.has_value());
    // The widget-owned part kind is not in the parser's tag table, so it reads as an unknown tag —
    // the same rejection an authored <ScrollBar> takes.
    CHECK(doc.error().find("DropdownArrow") != string::npos);
    CHECK(doc.error().find("unknown element tag") != string::npos);
}
