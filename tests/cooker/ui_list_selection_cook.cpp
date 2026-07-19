// Selectable-list cook test: cooks a fixture pack whose UIDocument authors a <List
// selection="extended"> over a composite item template (a Panel wrapping a Text, an Image, and a
// Button) against a stylesheet carrying a `.row:selected` rule. Asserts the cook succeeds — a
// `selection` attribute is a recognized widget-config attribute and `:selected` a recognized
// pseudo-state, neither a located error — that the mode reaches the recipe's binding table
// verbatim, that the item template survives as an arbitrary subtree, and that the selected variant
// cooks with the Selected state bit the runtime resolves it by.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/StyleProperty.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId StyleSheetId{0x9EF58640F59CF646ULL};
    constexpr AssetId UIDocumentId{0xFA23CAC896AC45A8ULL};
}

TEST_CASE("Cooker: a selectable List and a :selected rule cook through the authoring surface")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_list_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_ui_list.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE_MESSAGE(cookResult.has_value(),
                    "cook failed: ", cookResult ? string{} : cookResult.error());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    SUBCASE("the document records the selection mode and keeps the composite item template")
    {
        const optional<ArchiveEntry> docEntry = reader->Find(UIDocumentId);
        REQUIRE(docEntry.has_value());
        CHECK(docEntry->Type == AssetTypes::UIDocument);

        const std::span<const u8> blob = docEntry->Blob;
        CookedUIDocumentHeader header{};
        REQUIRE(blob.size() >= sizeof(header));
        std::memcpy(&header, blob.data(), sizeof(header));
        CHECK(header.Version == CookedUIDocumentVersion);

        // Root Panel, the List, the row Panel, and its three item children.
        REQUIRE(header.ElementCount == 6);

        usize offset = sizeof(CookedUIDocumentHeader) +
                       static_cast<usize>(header.StyleSheetCount) * sizeof(u64);
        vector<CookedUIElement> elements(header.ElementCount);
        for (CookedUIElement& element : elements)
        {
            std::memcpy(&element, blob.data() + offset, sizeof(element));
            offset += sizeof(CookedUIElement);
        }
        offset += static_cast<usize>(header.ClassCount) * sizeof(CookedUIStringSpan);
        vector<CookedUIBinding> bindings(header.BindingCount);
        for (CookedUIBinding& binding : bindings)
        {
            std::memcpy(&binding, blob.data() + offset, sizeof(binding));
            offset += sizeof(CookedUIBinding);
        }
        offset += static_cast<usize>(header.HandlerCount) * sizeof(CookedUIHandler);
        offset += static_cast<usize>(header.InlinePropertyCount) * sizeof(CookedStyleProperty);
        const std::span<const u8> strings = blob.subspan(offset);
        const auto read = [&](const CookedUIStringSpan& span)
        {
            return string(reinterpret_cast<const char*>(strings.data()) + span.Offset, span.Length);
        };

        // The item template is an arbitrary subtree, not a text leaf: the List's one child is a
        // Panel holding a Text, an Image, and a Button.
        const CookedUIElement& list = elements[1];
        CHECK(list.Kind == static_cast<u32>(Gui::ElementKind::List));
        CHECK(read(list.Id) == "tracks");
        CHECK(list.ChildCount == 1);
        CHECK(elements[2].Kind == static_cast<u32>(Gui::ElementKind::Panel));
        CHECK(elements[2].ChildCount == 3);
        CHECK(elements[3].Kind == static_cast<u32>(Gui::ElementKind::Text));
        CHECK(elements[4].Kind == static_cast<u32>(Gui::ElementKind::Image));
        CHECK(elements[5].Kind == static_cast<u32>(Gui::ElementKind::Button));

        // `selection` rides the binding table verbatim, like every other widget-config attribute;
        // the runtime widget layer parses the mode at Instantiate.
        string mode;
        for (u32 i = 0; i < list.BindingCount; ++i)
        {
            const CookedUIBinding& binding = bindings[list.FirstBinding + i];
            if (read(binding.Property) == "selection")
            {
                mode = read(binding.Expression);
            }
        }
        CHECK(mode == "extended");
    }

    SUBCASE("the stylesheet carries a Selected-scoped variant beside its hover variant")
    {
        const optional<ArchiveEntry> sheetEntry = reader->Find(StyleSheetId);
        REQUIRE(sheetEntry.has_value());
        CHECK(sheetEntry->Type == AssetTypes::StyleSheet);

        const std::span<const u8> blob = sheetEntry->Blob;
        CookedStyleSheetHeader header{};
        REQUIRE(blob.size() >= sizeof(header));
        std::memcpy(&header, blob.data(), sizeof(header));
        CHECK(header.Version == CookedStyleSheetVersion);

        vector<CookedStyleRule> rules(header.RuleCount);
        usize offset = sizeof(CookedStyleSheetHeader);
        for (CookedStyleRule& rule : rules)
        {
            std::memcpy(&rule, blob.data() + offset, sizeof(rule));
            offset += sizeof(rule);
        }

        // `.row:selected` cooked to a rule scoped to exactly the Selected bit — the same shape
        // `:hover` takes, so the runtime's variant fold needs no selection-specific path.
        bool foundSelected = false;
        bool foundHover = false;
        for (const CookedStyleRule& rule : rules)
        {
            if (string(rule.Class) != "row")
            {
                continue;
            }
            if (rule.State == static_cast<u32>(Gui::ElementState::Selected))
            {
                foundSelected = true;
                CHECK(rule.PropertyCount == 2);
            }
            if (rule.State == static_cast<u32>(Gui::ElementState::Hovered))
            {
                foundHover = true;
            }
        }
        CHECK(foundSelected);
        CHECK(foundHover);

        // The widget-owned scrollbar parts are selected by ordinary type and type.class selectors,
        // and take a pseudo-state variant like any other element — the whole point of making them
        // real elements rather than painted-in widget internals.
        bool foundBar = false;
        bool foundNarrowBar = false;
        bool foundThumbHover = false;
        for (const CookedStyleRule& rule : rules)
        {
            if (string(rule.Type) == "ScrollBar" && string(rule.Class).empty())
            {
                foundBar = true;
            }
            if (string(rule.Type) == "ScrollBar" && string(rule.Class) == "vertical")
            {
                foundNarrowBar = true;
            }
            if (string(rule.Type) == "ScrollBarThumb" &&
                rule.State == static_cast<u32>(Gui::ElementState::Hovered))
            {
                foundThumbHover = true;
            }
        }
        CHECK(foundBar);
        CHECK(foundNarrowBar);
        CHECK(foundThumbHover);
    }

    SUBCASE("the per-axis overflow longhands and the scrollbar layout cook onto the list rule")
    {
        const optional<ArchiveEntry> sheetEntry = reader->Find(StyleSheetId);
        REQUIRE(sheetEntry.has_value());

        const std::span<const u8> blob = sheetEntry->Blob;
        CookedStyleSheetHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));

        vector<CookedStyleRule> rules(header.RuleCount);
        usize offset = sizeof(CookedStyleSheetHeader);
        for (CookedStyleRule& rule : rules)
        {
            std::memcpy(&rule, blob.data() + offset, sizeof(rule));
            offset += sizeof(rule);
        }
        vector<CookedStyleProperty> properties(header.PropertyCount);
        for (CookedStyleProperty& property : properties)
        {
            std::memcpy(&property, blob.data() + offset, sizeof(property));
            offset += sizeof(property);
        }

        // `.track-list` authors the axes independently — the case a single `overflow` keyword
        // cannot express, and the reason a vertical list does not drift sideways.
        optional<u32> overflowX;
        optional<u32> overflowY;
        optional<u32> scrollbar;
        for (const CookedStyleRule& rule : rules)
        {
            if (string(rule.Class) != "track-list")
            {
                continue;
            }
            for (u32 i = 0; i < rule.PropertyCount; ++i)
            {
                const CookedStyleProperty& property = properties[rule.FirstProperty + i];
                switch (static_cast<Gui::StyleProperty>(property.Property))
                {
                case Gui::StyleProperty::OverflowX:
                    overflowX = property.Unit;
                    break;
                case Gui::StyleProperty::OverflowY:
                    overflowY = property.Unit;
                    break;
                case Gui::StyleProperty::ScrollbarLayout:
                    scrollbar = property.Unit;
                    break;
                default:
                    break;
                }
            }
        }
        CHECK(overflowX == optional<u32>(static_cast<u32>(Gui::Overflow::Hidden)));
        CHECK(overflowY == optional<u32>(static_cast<u32>(Gui::Overflow::Scroll)));
        CHECK(scrollbar == optional<u32>(static_cast<u32>(Gui::ScrollbarLayout::Gutter)));
    }

    std::filesystem::remove(outArchive);
}
