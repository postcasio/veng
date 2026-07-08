// UI-document markup-repetition cook test: covers the `count` unroll and `${}` index
// substitution the importer applies before emission. Cooks XML sources written to a temp dir and
// decodes the resulting element tree, asserting: a `count="N"` element and its whole subtree are
// replicated N times in pre-order; `${i}`/`${n}`/`${n:0W}` substitute into id/class/text and
// binding-expression positions; the descendant of a repeated element inherits the repeat index;
// `$${` escapes a literal `${`. Plus every located error: `count` on the root, a nested `count`,
// an out-of-range N, a malformed `${…}`, and a stray `${` outside any repeat.

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

    // The decoded pieces of a cooked UI-document blob a repetition test inspects: the pre-order
    // element records and the tables it resolves their string spans and bindings through.
    struct DecodedDocument
    {
        CookedUIDocumentHeader Header{};
        vector<CookedUIElement> Elements;
        vector<CookedUIStringSpan> Classes;
        vector<CookedUIBinding> Bindings;
        vector<u8> Strings;

        [[nodiscard]] string Read(const CookedUIStringSpan& span) const
        {
            return string(reinterpret_cast<const char*>(Strings.data()) + span.Offset, span.Length);
        }
    };

    // Cooks a single-UIDocument pack whose source is `markup`, written to the process temp dir,
    // returning the decoded document or the cook's located error.
    Result<DecodedDocument> CookMarkup(std::string_view markup)
    {
        std::random_device rng;
        const string stem = fmt::format("veng_ui_repeat_{:08x}", rng());
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

        usize offset = sizeof(CookedUIDocumentHeader) +
                       static_cast<usize>(decoded.Header.StyleSheetCount) * sizeof(u64);
        decoded.Elements.resize(decoded.Header.ElementCount);
        for (CookedUIElement& element : decoded.Elements)
        {
            std::memcpy(&element, blob.data() + offset, sizeof(element));
            offset += sizeof(CookedUIElement);
        }
        decoded.Classes.resize(decoded.Header.ClassCount);
        for (CookedUIStringSpan& span : decoded.Classes)
        {
            std::memcpy(&span, blob.data() + offset, sizeof(span));
            offset += sizeof(CookedUIStringSpan);
        }
        decoded.Bindings.resize(decoded.Header.BindingCount);
        for (CookedUIBinding& binding : decoded.Bindings)
        {
            std::memcpy(&binding, blob.data() + offset, sizeof(binding));
            offset += sizeof(CookedUIBinding);
        }
        offset += static_cast<usize>(decoded.Header.HandlerCount) * sizeof(CookedUIHandler);
        offset +=
            static_cast<usize>(decoded.Header.InlinePropertyCount) * sizeof(CookedStyleProperty);
        decoded.Strings.assign(blob.begin() + static_cast<std::ptrdiff_t>(offset), blob.end());

        std::filesystem::remove(outArchive);
        return decoded;
    }
}

TEST_CASE("ui document repeat: count unrolls a composite subtree with ${} substitution")
{
    // A count="3" composite marker: three copies of the Panel and its Text child, in pre-order,
    // with the repeat index substituted into the marker id (${i}, 0-based) and its binding
    // expression, and into the child class/id/text (${n}/${n:02}) — the child inheriting its
    // parent's repeat index. `$${` in the text escapes a literal `${`.
    const Result<DecodedDocument> doc = CookMarkup(
        R"(<Panel id="pool">
             <Panel class="marker" id="mark-${i}" data="{row${i}}" count="3">
               <Text class="row-${n}" id="label-${n:02}">Item ${n} of $${total}</Text>
             </Panel>
           </Panel>)");
    REQUIRE_MESSAGE(doc.has_value(), "cook failed: ", doc ? string{} : doc.error());

    // Root Panel plus three (marker Panel + label Text) = seven elements, in pre-order:
    // pool, marker0, label0, marker1, label1, marker2, label2.
    REQUIRE(doc->Elements.size() == 7);
    CHECK(doc->Header.ElementCount == 7);
    CHECK(doc->Elements[0].ChildCount == 3);
    CHECK(doc->Read(doc->Elements[0].Id) == "pool");

    for (u32 index = 0; index < 3; ++index)
    {
        const CookedUIElement& marker = doc->Elements[1 + index * 2];
        const CookedUIElement& label = doc->Elements[2 + index * 2];

        CHECK(marker.Kind == static_cast<u32>(Gui::ElementKind::Panel));
        CHECK(marker.ChildCount == 1);
        // ${i} — the 0-based repeat index — landed in the marker id and its binding expression.
        CHECK(doc->Read(marker.Id) == fmt::format("mark-{}", index));
        REQUIRE(marker.ClassCount == 1);
        CHECK(doc->Read(doc->Classes[marker.FirstClass]) == "marker");
        REQUIRE(marker.BindingCount == 1);
        CHECK(doc->Read(doc->Bindings[marker.FirstBinding].Property) == "data");
        CHECK(doc->Read(doc->Bindings[marker.FirstBinding].Expression) ==
              fmt::format("row{}", index));

        // The child inherited the marker's repeat index: ${n} (1-based) and the zero-padded
        // ${n:02} substituted into its class, id, and text; $${ left a literal ${.
        CHECK(label.Kind == static_cast<u32>(Gui::ElementKind::Text));
        REQUIRE(label.ClassCount == 1);
        CHECK(doc->Read(doc->Classes[label.FirstClass]) == fmt::format("row-{}", index + 1));
        CHECK(doc->Read(label.Id) == fmt::format("label-{:02}", index + 1));
        CHECK(doc->Read(label.Text) == fmt::format("Item {} of ${{total}}", index + 1));
    }
}

TEST_CASE("ui document repeat: count of one is a single in-place copy")
{
    const Result<DecodedDocument> doc =
        CookMarkup(R"(<Panel><Text class="only" count="1">just ${n}</Text></Panel>)");
    REQUIRE_MESSAGE(doc.has_value(), "cook failed: ", doc ? string{} : doc.error());
    REQUIRE(doc->Elements.size() == 2);
    CHECK(doc->Elements[0].ChildCount == 1);
    CHECK(doc->Read(doc->Elements[1].Text) == "just 1");
}

TEST_CASE("ui document repeat: count on the root is a located error")
{
    const Result<DecodedDocument> doc = CookMarkup(R"(<Panel count="2"><Text>x</Text></Panel>)");
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().find("root") != string::npos);
}

TEST_CASE("ui document repeat: a nested count is a located error")
{
    const Result<DecodedDocument> doc =
        CookMarkup(R"(<Panel><Panel count="2"><Text count="3">x</Text></Panel></Panel>)");
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().find("nested") != string::npos);
}

TEST_CASE("ui document repeat: an out-of-range count is a located error")
{
    const Result<DecodedDocument> tooMany =
        CookMarkup(R"(<Panel><Text count="2000">x</Text></Panel>)");
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().find("range") != string::npos);

    const Result<DecodedDocument> zero = CookMarkup(R"(<Panel><Text count="0">x</Text></Panel>)");
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error().find("range") != string::npos);

    const Result<DecodedDocument> notNumber =
        CookMarkup(R"(<Panel><Text count="lots">x</Text></Panel>)");
    REQUIRE_FALSE(notNumber.has_value());
    CHECK(notNumber.error().find("integer") != string::npos);
}

TEST_CASE("ui document repeat: a malformed ${…} is a located error")
{
    const Result<DecodedDocument> unknownName =
        CookMarkup(R"(<Panel><Text count="2">${x}</Text></Panel>)");
    REQUIRE_FALSE(unknownName.has_value());

    const Result<DecodedDocument> badPad =
        CookMarkup(R"(<Panel><Text count="2">${n:2}</Text></Panel>)");
    REQUIRE_FALSE(badPad.has_value());

    const Result<DecodedDocument> unterminated =
        CookMarkup(R"(<Panel><Text count="2">${n</Text></Panel>)");
    REQUIRE_FALSE(unterminated.has_value());
}

TEST_CASE("ui document repeat: a ${ outside a count subtree is a located error")
{
    const Result<DecodedDocument> doc = CookMarkup(R"(<Panel><Text>${n}</Text></Panel>)");
    REQUIRE_FALSE(doc.has_value());
    CHECK(doc.error().find("outside") != string::npos);
}
