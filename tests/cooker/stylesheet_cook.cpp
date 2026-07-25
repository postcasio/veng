// Stylesheet variable cook test: cooks a `.vuss` sheet that uses @use imports, var() substitution
// (in rule values, a gradient stop list, and a @keyframes block), last-wins redefinition, and a
// variable referencing another variable, then checks the flattened rules carry the substituted
// values, the runtime variable table answers FindVariableColor/FindVariableScalar for the sheet's
// own color/scalar variables (and nullopt for a multi-token or @use'd one), the @use target is
// recorded as a build dependency, and each malformed sheet is a located cook error.

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/StyleProperty.h>
#include <Veng/Gui/StyleSheet.h>

#include "Asset/Loaders/StyleSheetLoader.h"
#include "Importers/StyleParse.h"

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
    CHECK(sheetEntry->Type == AssetTypes::StyleSheet);

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
    // and var(--accent) resolves to the @use'd theme color. The gradient stop list substitutes in
    // .panel-ramp, a rule of its own because a background fill source is exclusive.
    const Gui::StyleRule* panel = FindRuleByClass(decoded->Rules, "panel");
    REQUIRE(panel != nullptr);
    const Gui::StyleRule* panelRamp = FindRuleByClass(decoded->Rules, "panel-ramp");
    REQUIRE(panelRamp != nullptr);
    const Gui::StyleDeclaration* panelBg = FindDecl(*panel, Gui::StyleProperty::Background);
    const Gui::StyleDeclaration* panelColor = FindDecl(*panel, Gui::StyleProperty::TextColor);
    const Gui::StyleDeclaration* panelOpacity = FindDecl(*panel, Gui::StyleProperty::Opacity);
    const Gui::StyleDeclaration* panelGradient =
        FindDecl(*panelRamp, Gui::StyleProperty::BackgroundGradient);
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

    // An HDR variable: a functional rgb() color, unclamped, reaching the table intact — and the
    // rule that substituted it holds the same value as the literal control.
    const Gui::StyleRule* controlHdr = FindRuleByClass(decoded->Rules, "control-hdr");
    const Gui::StyleRule* panelHdr = FindRuleByClass(decoded->Rules, "panel-hdr");
    REQUIRE(controlHdr != nullptr);
    REQUIRE(panelHdr != nullptr);
    const Gui::StyleDeclaration* controlHdrColor =
        FindDecl(*controlHdr, Gui::StyleProperty::TextColor);
    const Gui::StyleDeclaration* panelHdrColor = FindDecl(*panelHdr, Gui::StyleProperty::TextColor);
    REQUIRE(controlHdrColor != nullptr);
    REQUIRE(panelHdrColor != nullptr);
    CheckColorEqual(panelHdrColor->Values, controlHdrColor->Values);
    const optional<vec4> hdrGlow = sheet->FindVariableColor("hdr-glow");
    REQUIRE(hdrGlow.has_value());
    CHECK(hdrGlow->g == doctest::Approx(3.0f));
    CHECK(hdrGlow->b == doctest::Approx(4.0f));
    CheckColorEqual(*hdrGlow, controlHdrColor->Values);

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
    const Result<vector<u8>> bytes =
        cooker.CookSource(source, AssetId{0x1}, AssetTypes::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("undefined variable") != string::npos);
}

TEST_CASE("Cooker: a '--' variable declared inside a rule is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_var_in_rule.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes =
        cooker.CookSource(source, AssetId{0x1}, AssetTypes::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("file-scope") != string::npos);
}

TEST_CASE("Cooker: an @use after a rule is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_use_after_rule.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes =
        cooker.CookSource(source, AssetId{0x1}, AssetTypes::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("@use") != string::npos);
}

TEST_CASE("Cooker: an @use naming a missing file is a located error")
{
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "style" / "err_use_missing.vuss";
    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const Result<vector<u8>> bytes =
        cooker.CookSource(source, AssetId{0x1}, AssetTypes::StyleSheet);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().find("cannot be opened") != string::npos);
}

TEST_CASE("Cooker: rgb()/rgba() author unclamped linear colors, distinct from hex")
{
    const string located = "loc";

    // rgb() takes three linear components; alpha defaults to 1; a component may exceed 1 (HDR).
    const Result<CookedStyleProperty> cyan =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "rgb(0, 4, 4)", located);
    REQUIRE(cyan.has_value());
    CHECK(cyan->Values[0] == doctest::Approx(0.0f));
    CHECK(cyan->Values[1] == doctest::Approx(4.0f));
    CHECK(cyan->Values[2] == doctest::Approx(4.0f));
    CHECK(cyan->Values[3] == doctest::Approx(1.0f));

    // rgba() takes an explicit alpha; surrounding whitespace is tolerated.
    const Result<CookedStyleProperty> bright =
        ParseStyleDeclaration(Gui::StyleProperty::Background, "rgba( 8 , 8 , 8 , 0.5 )", located);
    REQUIRE(bright.has_value());
    CHECK(bright->Values[0] == doctest::Approx(8.0f));
    CHECK(bright->Values[1] == doctest::Approx(8.0f));
    CHECK(bright->Values[2] == doctest::Approx(8.0f));
    CHECK(bright->Values[3] == doctest::Approx(0.5f));

    // A linear rgb() grey is taken directly, distinct from the same-looking sRGB hex (~0.216 linear).
    const Result<CookedStyleProperty> linearGrey =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "rgb(0.5, 0.5, 0.5)", located);
    const Result<CookedStyleProperty> hexGrey =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "#808080", located);
    REQUIRE(linearGrey.has_value());
    REQUIRE(hexGrey.has_value());
    CHECK(linearGrey->Values[0] == doctest::Approx(0.5f));
    CHECK(hexGrey->Values[0] == doctest::Approx(0.2158605f).epsilon(0.001));
    CHECK(linearGrey->Values[0] != doctest::Approx(hexGrey->Values[0]));

    // The standalone color entry (gradient-stop path) accepts the same syntax.
    const Result<vec4> stop = ParseStyleColor("rgb(2, 0, 0)", located);
    REQUIRE(stop.has_value());
    CHECK(stop->r == doctest::Approx(2.0f));
    CHECK(stop->a == doctest::Approx(1.0f));
}

TEST_CASE("Cooker: malformed rgb()/rgba() colors are located errors")
{
    const string located = "loc";

    // Wrong arity — rgb() needs exactly three components.
    const Result<CookedStyleProperty> few =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "rgb(1, 2)", located);
    REQUIRE_FALSE(few.has_value());
    CHECK(few.error().find("loc") != string::npos);

    // Too many components for rgba().
    const Result<CookedStyleProperty> many =
        ParseStyleDeclaration(Gui::StyleProperty::Background, "rgba(1, 2, 3, 4, 5)", located);
    REQUIRE_FALSE(many.has_value());
    CHECK(many.error().find("loc") != string::npos);

    // A non-numeric component.
    const Result<CookedStyleProperty> notNumber =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "rgb(1, x, 3)", located);
    REQUIRE_FALSE(notNumber.has_value());
    CHECK(notNumber.error().find("loc") != string::npos);

    // A negative component is rejected.
    const Result<CookedStyleProperty> negative =
        ParseStyleDeclaration(Gui::StyleProperty::TextColor, "rgb(1, -2, 3)", located);
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().find(">= 0") != string::npos);
}

TEST_CASE("Cooker: an rgb() gradient bakes an HDR ramp preserving >1 stops")
{
    // A gradient whose stops are authored with rgb() > 1 bakes into the RGBA16Sfloat ramp; the
    // half-float texels preserve the emissive magnitude a clamped RGBA8 ramp could not hold.
    const path source = Veng::TestSupport::TempDir() / "veng_hdr_gradient.vuss";
    {
        std::ofstream out(source);
        out << ".glow {\n"
               "  background-gradient: linear, rgb(0, 4, 4) 0%, rgb(8, 8, 8) 100%;\n"
               "}\n";
    }

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const AssetId sheetId{0x1};
    const Result<vector<u8>> bytes = cooker.CookSource(source, sheetId, AssetTypes::StyleSheet);
    REQUIRE(bytes.has_value());

    // CookSource yields an in-memory archive; unwrap the sheet entry's cooked blob to decode.
    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> sheetEntry = reader->Find(sheetId);
    REQUIRE(sheetEntry.has_value());

    CookedStyleSheetHeader sheetHeader{};
    std::memcpy(&sheetHeader, sheetEntry->Blob.data(), sizeof(sheetHeader));
    CHECK(sheetHeader.Version == CookedStyleSheetVersion);

    const AssetResult<Detail::DecodedStyleSheet> decoded =
        Detail::DecodeStyleSheet(sheetId, sheetEntry->Blob);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->Gradients.size() == 1);

    const Gui::StyleGradient& gradient = decoded->Gradients.front();
    REQUIRE(gradient.Width >= 2);
    REQUIRE(gradient.Ramp.size() == static_cast<usize>(gradient.Width) * 4 * sizeof(u16));

    const auto texel = [&gradient](u32 index) -> vec4
    {
        u16 halves[4] = {};
        std::memcpy(halves, gradient.Ramp.data() + static_cast<usize>(index) * 4 * sizeof(u16),
                    sizeof(halves));
        return vec4(glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]),
                    glm::unpackHalf1x16(halves[2]), glm::unpackHalf1x16(halves[3]));
    };

    // The first texel is the 0% stop rgb(0, 4, 4); the last is the 100% stop rgb(8, 8, 8). Both
    // exceed 1.0, which the widened ramp preserves (half-float precision, so a small tolerance).
    const vec4 first = texel(0);
    const vec4 last = texel(gradient.Width - 1);
    CHECK(first.g == doctest::Approx(4.0f).epsilon(0.01));
    CHECK(first.b == doctest::Approx(4.0f).epsilon(0.01));
    CHECK(last.r == doctest::Approx(8.0f).epsilon(0.01));
    CHECK(last.g == doctest::Approx(8.0f).epsilon(0.01));

    std::filesystem::remove(source);
}

TEST_CASE("Cooker: an edge shorthand resolves top, right, bottom, left — the CSS order")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "style_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_style_edges.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cookResult = cooker.CookPack(packJson, outArchive, {}, nullptr, nullptr);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> sheetEntry = reader->Find(MainSheetId);
    REQUIRE(sheetEntry.has_value());

    const AssetResult<Detail::DecodedStyleSheet> decoded =
        Detail::DecodeStyleSheet(MainSheetId, sheetEntry->Blob);
    REQUIRE(decoded.has_value());

    const Gui::StyleRule* rule = FindRuleByClass(decoded->Rules, "edge-order");
    REQUIRE(rule != nullptr);

    // Resolve through the same ApplyDeclaration the runtime uses, so the assertion covers the
    // whole authored-text-to-Style path rather than the cooked payload's raw component order.
    Gui::Style style{};
    for (const Gui::StyleDeclaration& declaration : rule->Declarations)
    {
        Gui::ApplyDeclaration(style, declaration, nullptr);
    }

    // margin: 1px 2px 3px 4px
    CHECK(style.Margin.Top == doctest::Approx(1.0f));
    CHECK(style.Margin.Right == doctest::Approx(2.0f));
    CHECK(style.Margin.Bottom == doctest::Approx(3.0f));
    CHECK(style.Margin.Left == doctest::Approx(4.0f));

    // padding: 5px 6px 7px 8px
    CHECK(style.Padding.Top == doctest::Approx(5.0f));
    CHECK(style.Padding.Right == doctest::Approx(6.0f));
    CHECK(style.Padding.Bottom == doctest::Approx(7.0f));
    CHECK(style.Padding.Left == doctest::Approx(8.0f));

    // inset: 9px 10px 11px 12px
    CHECK(style.Inset.Top == doctest::Approx(9.0f));
    CHECK(style.Inset.Right == doctest::Approx(10.0f));
    CHECK(style.Inset.Bottom == doctest::Approx(11.0f));
    CHECK(style.Inset.Left == doctest::Approx(12.0f));

    // corner-radius: 13px 14px 15px 16px — the radius shorthand runs clockwise from the top-left
    // corner, matching CSS border-radius, and is unaffected by the edge order.
    CHECK(style.Radii.TopLeft == doctest::Approx(13.0f));
    CHECK(style.Radii.TopRight == doctest::Approx(14.0f));
    CHECK(style.Radii.BottomRight == doctest::Approx(15.0f));
    CHECK(style.Radii.BottomLeft == doctest::Approx(16.0f));
}

TEST_CASE("Cooker: the background-image family parses into its cooked slots")
{
    const string located = "loc";

    // The texture rides the Handle slot, the same transport `font` uses.
    const Result<CookedStyleProperty> image =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundImage, "0x0123456789ABCDEF", located);
    REQUIRE(image.has_value());
    CHECK(image->Handle == 0x0123456789ABCDEFULL);

    // A non-id value is a located error, like a malformed `font`.
    const Result<CookedStyleProperty> notAnId =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundImage, "panel.png", located);
    REQUIRE_FALSE(notAnId.has_value());
    CHECK(notAnId.error().find("loc") != string::npos);

    // The slice insets take the four-edge shorthand in CSS order (top, right, bottom, left).
    const Result<CookedStyleProperty> slice =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundSlice, "1px 2px 3px 4px", located);
    REQUIRE(slice.has_value());
    CHECK(slice->Values[0] == doctest::Approx(1.0f));
    CHECK(slice->Values[1] == doctest::Approx(2.0f));
    CHECK(slice->Values[2] == doctest::Approx(3.0f));
    CHECK(slice->Values[3] == doctest::Approx(4.0f));

    // The two enums ride Unit, and an unknown keyword is a located error.
    const Result<CookedStyleProperty> fit =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundFit, "cover", located);
    REQUIRE(fit.has_value());
    CHECK(fit->Unit == static_cast<u32>(Gui::ImageFit::Cover));

    const Result<CookedStyleProperty> repeat =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundRepeat, "tile", located);
    REQUIRE(repeat.has_value());
    CHECK(repeat->Unit == static_cast<u32>(Gui::ImageRepeat::Tile));

    const Result<CookedStyleProperty> badFit =
        ParseStyleDeclaration(Gui::StyleProperty::BackgroundFit, "stretch", located);
    REQUIRE_FALSE(badFit.has_value());
    CHECK(badFit.error().find("loc") != string::npos);
}

TEST_CASE("Cooker: a block authoring two background fill sources is a located error")
{
    const string located = "loc";

    const auto declaration = [](Gui::StyleProperty property)
    {
        CookedStyleProperty cp{};
        cp.Property = static_cast<u32>(property);
        return cp;
    };

    // One source, repeated, is a plain last-wins override rather than a conflict.
    const vector<CookedStyleProperty> single = {declaration(Gui::StyleProperty::Background),
                                                declaration(Gui::StyleProperty::Background),
                                                declaration(Gui::StyleProperty::CornerRadius)};
    CHECK(CheckExclusiveFillSources(single, located).has_value());

    // Two different sources in one block cannot both be the fill, so the cook rejects it.
    const vector<CookedStyleProperty> both = {declaration(Gui::StyleProperty::Background),
                                              declaration(Gui::StyleProperty::BackgroundImage)};
    const VoidResult conflict = CheckExclusiveFillSources(both, located);
    REQUIRE_FALSE(conflict.has_value());
    CHECK(conflict.error().find("loc") != string::npos);
    CHECK(conflict.error().find("background-image") != string::npos);

    // The rule covers all four sources, not the two the shape path started with: a material is the
    // top of the order and conflicts with each of the three below it just as they conflict with
    // each other, so the diagnostic is what makes the runtime's exclusive `else if` chain the only
    // reachable behavior rather than a silent drop.
    const std::array<Gui::StyleProperty, 4> sources = {
        Gui::StyleProperty::BackgroundMaterial, Gui::StyleProperty::BackgroundGradient,
        Gui::StyleProperty::BackgroundImage, Gui::StyleProperty::Background};
    for (usize i = 0; i < sources.size(); ++i)
    {
        for (usize j = 0; j < sources.size(); ++j)
        {
            const vector<CookedStyleProperty> pair = {declaration(sources[i]),
                                                      declaration(sources[j])};
            const VoidResult result = CheckExclusiveFillSources(pair, located);
            CHECK(result.has_value() == (i == j));
        }
    }
}

TEST_CASE("Cooker: the Image fill family parses into its cooked slots")
{
    const string located = "loc";

    // `object-fit` and `image-repeat` are the widget-side twins of the background enums, so they
    // ride Unit against the identical vocabulary.
    const Result<CookedStyleProperty> fit =
        ParseStyleDeclaration(Gui::StyleProperty::ObjectFit, "contain", located);
    REQUIRE(fit.has_value());
    CHECK(fit->Unit == static_cast<u32>(Gui::ImageFit::Contain));

    const Result<CookedStyleProperty> repeat =
        ParseStyleDeclaration(Gui::StyleProperty::ImageRepeat, "tile", located);
    REQUIRE(repeat.has_value());
    CHECK(repeat->Unit == static_cast<u32>(Gui::ImageRepeat::Tile));

    // `image-slice` takes the four-edge shorthand in CSS order (top, right, bottom, left).
    const Result<CookedStyleProperty> slice =
        ParseStyleDeclaration(Gui::StyleProperty::ImageSlice, "1px 2px 3px 4px", located);
    REQUIRE(slice.has_value());
    CHECK(slice->Values[0] == doctest::Approx(1.0f));
    CHECK(slice->Values[1] == doctest::Approx(2.0f));
    CHECK(slice->Values[2] == doctest::Approx(3.0f));
    CHECK(slice->Values[3] == doctest::Approx(4.0f));

    // An unknown keyword is a located error rather than a silent default.
    const Result<CookedStyleProperty> badFit =
        ParseStyleDeclaration(Gui::StyleProperty::ObjectFit, "scale-down", located);
    REQUIRE_FALSE(badFit.has_value());
    CHECK(badFit.error().find("loc") != string::npos);

    // The Image fill properties are not background fill *sources*, so authoring them beside a
    // background never trips the exclusivity rule.
    const auto declaration = [](Gui::StyleProperty property)
    {
        CookedStyleProperty cp{};
        cp.Property = static_cast<u32>(property);
        return cp;
    };
    const vector<CookedStyleProperty> beside = {declaration(Gui::StyleProperty::Background),
                                                declaration(Gui::StyleProperty::ObjectFit),
                                                declaration(Gui::StyleProperty::ImageSlice)};
    CHECK(CheckExclusiveFillSources(beside, located).has_value());
}

TEST_CASE("Cooker: the box-shadow shorthand splits into a geometry and a color declaration")
{
    const string located = "loc";

    // The full form: offset, blur, spread, color, then the inset keyword.
    const Result<vector<CookedStyleProperty>> full =
        ParseBoxShadowDeclaration("2px 3px 8px 1px rgba(0.1, 0.2, 0.3, 0.5) inset", located);
    REQUIRE(full.has_value());
    REQUIRE(full->size() == 2);
    CHECK((*full)[0].Property == static_cast<u32>(Gui::StyleProperty::BoxShadow));
    CHECK((*full)[0].Unit == static_cast<u32>(Gui::BoxShadowMode::Inset));
    CHECK((*full)[0].Values[0] == doctest::Approx(2.0f));
    CHECK((*full)[0].Values[1] == doctest::Approx(3.0f));
    CHECK((*full)[0].Values[2] == doctest::Approx(8.0f));
    CHECK((*full)[0].Values[3] == doctest::Approx(1.0f));
    CHECK((*full)[1].Property == static_cast<u32>(Gui::StyleProperty::BoxShadowColor));
    CHECK((*full)[1].Values[0] == doctest::Approx(0.1f));
    CHECK((*full)[1].Values[3] == doctest::Approx(0.5f));

    // Blur, spread, and color are all optional; an omitted color is opaque black.
    const Result<vector<CookedStyleProperty>> minimal = ParseBoxShadowDeclaration("4 5", located);
    REQUIRE(minimal.has_value());
    REQUIRE(minimal->size() == 2);
    CHECK((*minimal)[0].Unit == static_cast<u32>(Gui::BoxShadowMode::Drop));
    CHECK((*minimal)[0].Values[2] == doctest::Approx(0.0f));
    CHECK((*minimal)[0].Values[3] == doctest::Approx(0.0f));
    CHECK((*minimal)[1].Values[3] == doctest::Approx(1.0f));

    // `none` clears the shadow, so it cooks the geometry declaration alone.
    const Result<vector<CookedStyleProperty>> none = ParseBoxShadowDeclaration("none", located);
    REQUIRE(none.has_value());
    REQUIRE(none->size() == 1);
    CHECK((*none)[0].Unit == static_cast<u32>(Gui::BoxShadowMode::None));

    // Too few lengths is a located error rather than a silently defaulted offset.
    const Result<vector<CookedStyleProperty>> short_ = ParseBoxShadowDeclaration("4px", located);
    REQUIRE_FALSE(short_.has_value());
    CHECK(short_.error().find("loc") != string::npos);
}
