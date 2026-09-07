// UI-document component-splice cook test: cooks a fixture pack whose host document embeds a
// reusable fragment twice with <Component src=… param:…>. Asserts the cooker splices the
// fragment's element subtree under an ElementKind::Component boundary (recording the fragment's
// AssetId, its driver id left unbound), applies each embed's ${param} substitution and the
// fragment's own ${i} count, keeps the two embeds independent, folds the fragment's stylesheet /
// font / texture dependencies into the host, and rejects a self-embedding fragment as a cook error
// naming the chain.

#include <algorithm>
#include <array>
#include <filesystem>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>

#include "Asset/Loaders/UIDocumentLoader.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId FontId{0xFB6782CABF076640ULL};
    constexpr AssetId TextureId{0x4DE87D318251BC1EULL};
    constexpr AssetId SheetId{0x5B33ACBEC98E1BD8ULL};
    constexpr AssetId FragmentId{0x00C0FFEE0BAD6E01ULL};
    constexpr AssetId HostId{0x00C0FFEE01057001ULL};
    // The scoped driver id the host's first <Component> names; the second names none.
    constexpr u64 DriverId = 0x00C0FFEE0D817E01ULL;

    bool Contains(const vector<AssetId>& ids, AssetId id)
    {
        return std::ranges::any_of(ids, [id](AssetId x) { return x.Value == id.Value; });
    }
}

TEST_CASE("Cooker: a <Component> splices a fragment subtree under a boundary with params applied")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_component_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_ui_component.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    const string cookError = cookResult.has_value() ? string{} : cookResult.error();
    REQUIRE_MESSAGE(cookResult.has_value(), cookError);

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> hostEntry = reader->Find(HostId);
    REQUIRE(hostEntry.has_value());

    const AssetResult<Detail::DecodedUIDocument> decoded =
        Detail::DecodeUIDocument(HostId, hostEntry->Blob);
    REQUIRE(decoded.has_value());
    const vector<Gui::UIElementRecipe>& elements = decoded->Elements;

    // The two <Component>s each became an ElementKind::Component boundary recording the fragment's
    // AssetId, with the fragment's single root as its one child.
    vector<usize> boundaries;
    for (usize i = 0; i < elements.size(); ++i)
    {
        if (elements[i].Kind == Gui::ElementKind::Component)
        {
            boundaries.push_back(i);
        }
    }
    REQUIRE(boundaries.size() == 2);

    // The first embed names a scoped driver; the second names none, so it stays pure shared markup.
    CHECK(elements[boundaries[0]].ComponentDriver == DriverId);
    CHECK(elements[boundaries[1]].ComponentDriver == 0);

    const std::array<string, 2> titles{"Ready", "Set"};
    for (usize b = 0; b < boundaries.size(); ++b)
    {
        const usize idx = boundaries[b];
        const Gui::UIElementRecipe& boundary = elements[idx];
        CHECK(boundary.ComponentSource.Value == FragmentId.Value);
        CHECK(boundary.ChildCount == 1);

        // The spliced subtree: the fragment's Panel root, its ${title} label (this embed's param),
        // its Image, and its two count-unrolled dots (${i} resolved by the fragment's own count).
        REQUIRE(idx + 5 < elements.size());
        CHECK(elements[idx + 1].Kind == Gui::ElementKind::Panel);
        CHECK(std::ranges::find(elements[idx + 1].Classes, string("badge")) !=
              elements[idx + 1].Classes.end());
        CHECK(elements[idx + 2].Kind == Gui::ElementKind::Text);
        CHECK(elements[idx + 2].Text == titles[b]);
        CHECK(elements[idx + 3].Kind == Gui::ElementKind::Image);
        CHECK(elements[idx + 3].Src.Value == TextureId.Value);
        CHECK(elements[idx + 4].Text == "dot-0");
        CHECK(elements[idx + 5].Text == "dot-1");
    }

    // Independence: the two embeds carry different param text, so one embed's substitution does not
    // bleed into the other.
    CHECK(elements[boundaries[0] + 2].Text != elements[boundaries[1] + 2].Text);

    // The fragment's stylesheet, font, and texture dependencies fold into the host's set, even
    // though the host root declared none of them — so the host eager-loads them like inline markup.
    CHECK(Contains(decoded->StyleSheetIds, SheetId));
    CHECK(Contains(decoded->FontIds, FontId));
    CHECK(Contains(decoded->TextureIds, TextureId));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a self-embedding <Component> is a cook error naming the chain")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_component_cycle_pack.json";
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_ui_component_cycle.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE_FALSE(cookResult.has_value());
    CHECK(cookResult.error().find("cycle") != string::npos);
    CHECK(cookResult.error().find("component_cycle.vui.xml") != string::npos);
}
