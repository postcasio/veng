// Stylesheet variable cook test: cooks a `.vuss` sheet that uses @use imports, var() substitution
// (in rule values, a gradient stop list, and a @keyframes block), last-wins redefinition, and a
// variable referencing another variable, then checks the flattened rules carry the substituted
// values, the runtime variable table answers FindVariableColor/FindVariableScalar for the sheet's
// own color/scalar variables (and nullopt for a multi-token or @use'd one), the @use target is
// recorded as a build dependency, and each malformed sheet is a located cook error.

#include <algorithm>
#include <filesystem>
#include <span>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/StyleProperty.h>
#include <Veng/Gui/StyleSheet.h>

#include "Asset/Loaders/StyleSheetLoader.h"

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId MainSheetId{0x51A7EE7500000001ULL};

    const Gui::StyleRule* FindRuleByClass(const vector<Gui::StyleRule>& rules, string_view cls)
    {
        for (const Gui::StyleRule& rule : rules)
        {
            if (rule.Class == cls && rule.State == Gui::ElementState::None)
            {
                return &rule;
            }
        }
        return nullptr;
    }

    const Gui::StyleDeclaration* FindDecl(const Gui::StyleRule& rule, Gui::StyleProperty property)
    {
        for (const Gui::StyleDeclaration& declaration : rule.Declarations)
        {
            if (declaration.Property == property)
            {
                return &declaration;
            }
        }
        return nullptr;
    }

    void CheckColorEqual(const vec4& lhs, const vec4& rhs)
    {
        CHECK(lhs.r == doctest::Approx(rhs.r));
        CHECK(lhs.g == doctest::Approx(rhs.g));
        CHECK(lhs.b == doctest::Approx(rhs.b));
        CHECK(lhs.a == doctest::Approx(rhs.a));
    }
}

TEST_CASE("Cooker: stylesheet variables substitute, redefine last-wins, and fill the runtime table")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "style_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_style_vars.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // The @use target must land in the recorded dependency list so a theme edit re-cooks the sheet.
    vector<path> dependencies;
    const VoidResult cookResult =
        cooker.CookPack(packJson, outArchive, {}, nullptr, nullptr, &dependencies);
    REQUIRE(cookResult.has_value());

    const bool recordsTheme = std::ranges::any_of(dependencies, [](const path& dep)
                                                  { return dep.filename() == "theme.vuss"; });
    CHECK(recordsTheme);

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> sheetEntry = reader->Find(MainSheetId);
    REQUIRE(sheetEntry.has_value());
    CHECK(sheetEntry->Type == AssetType::StyleSheet);

    const std::span<const u8> blob = sheetEntry->Blob;
    REQUIRE(blob.size() >= sizeof(CookedStyleSheetHeader));
    CookedStyleSheetHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.Version == CookedStyleSheetVersion);

    const AssetResult<Detail::DecodedStyleSheet> decoded =
        Detail::DecodeStyleSheet(MainSheetId, blob);
    REQUIRE(decoded.has_value());

    // The control rules hold the literals the var() uses resolve to.
    const Gui::StyleRule* controlPanel = FindRuleByClass(decoded->Rules, "control-panel");
    const Gui::StyleRule* controlLate = FindRuleByClass(decoded->Rules, "control-late");
    const Gui::StyleRule* controlAccent = FindRuleByClass(decoded->Rules, "control-accent");
    REQUIRE(controlPanel != nullptr);
    REQUIRE(controlLate != nullptr);
    REQUIRE(controlAccent != nullptr);
    const Gui::StyleDeclaration* controlPanelBg =
        FindDecl(*controlPanel, Gui::StyleProperty::Background);
    const Gui::StyleDeclaration* controlLateBg =
        FindDecl(*controlLate, Gui::StyleProperty::Background);
    const Gui::StyleDeclaration* controlAccentColor =
        FindDecl(*controlAccent, Gui::StyleProperty::TextColor);
    REQUIRE(controlPanelBg != nullptr);
    REQUIRE(controlLateBg != nullptr);
    REQUIRE(controlAccentColor != nullptr);

    // .panel: var(--panel) resolves to the value at its use site (#10203a, before redefinition),
    // var(--accent) resolves to the @use'd theme color, and the gradient stop list substituted.
    const Gui::StyleRule* panel = FindRuleByClass(decoded->Rules, "panel");
    REQUIRE(panel != nullptr);
    const Gui::StyleDeclaration* panelBg = FindDecl(*panel, Gui::StyleProperty::Background);
    const Gui::StyleDeclaration* panelColor = FindDecl(*panel, Gui::StyleProperty::TextColor);
    const Gui::StyleDeclaration* panelOpacity = FindDecl(*panel, Gui::StyleProperty::Opacity);
    const Gui::StyleDeclaration* panelGradient =
        FindDecl(*panel, Gui::StyleProperty::BackgroundGradient);
    REQUIRE(panelBg != nullptr);
    REQUIRE(panelColor != nullptr);
    REQUIRE(panelOpacity != nullptr);
    REQUIRE(panelGradient != nullptr);
    CheckColorEqual(panelBg->Values, controlPanelBg->Values);
    CheckColorEqual(panelColor->Values, controlAccentColor->Values);
    CHECK(panelOpacity->Values.x == doctest::Approx(0.5f));
    // The gradient stop list parsed after substitution, baking a gradient into the sheet.
    CHECK(decoded->Gradients.size() >= 1);

    // .panel-late: var(--panel) resolves to the redefined value (#204080) at this later use site.
    const Gui::StyleRule* panelLate = FindRuleByClass(decoded->Rules, "panel-late");
    REQUIRE(panelLate != nullptr);
    const Gui::StyleDeclaration* panelLateBg = FindDecl(*panelLate, Gui::StyleProperty::Background);
    REQUIRE(panelLateBg != nullptr);
    CheckColorEqual(panelLateBg->Values, controlLateBg->Values);

    // The @keyframes block substituted var(--half) into the `from` keyframe's opacity.
    REQUIRE(decoded->Animations.size() == 1);
    REQUIRE(decoded->Animations[0].Keyframes.size() >= 1);
    const Gui::StyleKeyframe& first = decoded->Animations[0].Keyframes.front();
    CHECK(first.Offset == doctest::Approx(0.0f));
    const Gui::StyleDeclaration* keyframeOpacity = nullptr;
    for (const Gui::StyleDeclaration& declaration : first.Declarations)
    {
        if (declaration.Property == Gui::StyleProperty::Opacity)
        {
            keyframeOpacity = &declaration;
        }
    }
    REQUIRE(keyframeOpacity != nullptr);
    CHECK(keyframeOpacity->Values.x == doctest::Approx(0.5f));

    // The runtime variable table carries the sheet's own color/scalar variables only.
    const Ref<Gui::StyleSheet> sheet =
        Gui::StyleSheet::Create(std::move(decoded->Rules), std::move(decoded->Animations),
                                std::move(decoded->Gradients), std::move(decoded->Variables), {});

    // A color variable, and one that resolved through another variable (var(--accent)).
    const optional<vec4> accentStrong = sheet->FindVariableColor("accent-strong");
    REQUIRE(accentStrong.has_value());
    CheckColorEqual(*accentStrong, controlAccentColor->Values);

    // The redefined value is what the table keeps (last-wins).
    const optional<vec4> panelVar = sheet->FindVariableColor("panel");
    REQUIRE(panelVar.has_value());
    CheckColorEqual(*panelVar, controlLateBg->Values);

    // A scalar variable.
    const optional<f32> half = sheet->FindVariableScalar("half");
    REQUIRE(half.has_value());
    CHECK(*half == doctest::Approx(0.5f));

    // A multi-token variable is cook-time-only: no table entry of either kind.
    CHECK_FALSE(sheet->FindVariableColor("stops").has_value());
    CHECK_FALSE(sheet->FindVariableScalar("stops").has_value());

    // An @use'd variable belongs to the theme sheet, not this one.
    CHECK_FALSE(sheet->FindVariableColor("accent").has_value());
    CHECK_FALSE(sheet->FindVariableScalar("gap").has_value());

    // A typed lookup does not cross kinds.
    CHECK_FALSE(sheet->FindVariableScalar("accent-strong").has_value());
    CHECK_FALSE(sheet->FindVariableColor("half").has_value());

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a stylesheet var() naming an undefined variable is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_undefined.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes = cooker.CookSource(source, AssetId{0x1}, AssetType::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("undefined variable") != string::npos);
}

TEST_CASE("Cooker: a '--' variable declared inside a rule is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_var_in_rule.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes = cooker.CookSource(source, AssetId{0x1}, AssetType::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("file-scope") != string::npos);
}

TEST_CASE("Cooker: an @use after a rule is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_use_after_rule.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes = cooker.CookSource(source, AssetId{0x1}, AssetType::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("@use") != string::npos);
}

TEST_CASE("Cooker: an @use naming a missing file is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_use_missing.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes = cooker.CookSource(source, AssetId{0x1}, AssetType::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("cannot be opened") != string::npos);
}
