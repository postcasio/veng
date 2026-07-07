// UI-document + stylesheet load test: cooks a fixture pack with a Font, a StyleSheet, and a
// UIDocument that references both, mounts it, LoadSyncs the UIDocument, and asserts:
//  (a) the recipe element tree matches the markup structure, and its bindings/handlers are stored;
//  (b) the referenced stylesheet's resolved rules carry the expected base + :hover + :disabled
//      variants, with colors resolved to linear;
//  (c) the font/stylesheet dependencies resolved (the handles are live);
//  (d) Gui::Document::Instantiate yields two independent trees over one recipe.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Font.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Gui/UIDocument.h>

#include "Asset/Loaders/StyleSheetLoader.h"
#include "Asset/Loaders/UIDocumentLoader.h"

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    constexpr AssetId StyleSheetId{0x5B33ACBEC98E1BD8ULL};
    constexpr AssetId UIDocumentId{0x6DF03B87430D8A52ULL};
    constexpr AssetId FontId{0xFB6782CABF076640ULL};

    // Finds the first rule whose selector matches (type, class, id, state); nullptr if none.
    const Gui::StyleRule* FindRule(const Gui::StyleSheet& sheet, string_view type, string_view cls,
                                   string_view id, Gui::ElementState state)
    {
        for (const Gui::StyleRule& rule : sheet.GetRules())
        {
            if (rule.Type == type && rule.Class == cls && rule.Id == id && rule.State == state)
            {
                return &rule;
            }
        }
        return nullptr;
    }

    const Gui::StyleDeclaration* FindDecl(const Gui::StyleRule& rule, Gui::StyleProperty property)
    {
        for (const Gui::StyleDeclaration& decl : rule.Declarations)
        {
            if (decl.Property == property)
            {
                return &decl;
            }
        }
        return nullptr;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ui document: cook, load, verify recipe + resolved rules + deps + independence")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "ui_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // Loading the document eager-loads its stylesheet + font dependencies (the material shape).
    const AssetResult<AssetHandle<Gui::UIDocument>> handle =
        assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    REQUIRE_MESSAGE(handle.has_value(),
                    "load failed: ", handle ? "" : handle.error().Detail.c_str());
    REQUIRE(handle->IsLoaded());

    const Gui::UIDocument& recipe = *handle->Get();

    // (a) The recipe tree matches the markup: Panel > (Text#title, Text, ProgressBar, Slider,
    // Button).
    const vector<Gui::UIElementRecipe>& elements = recipe.GetElements();
    REQUIRE(elements.size() == 6);
    CHECK(elements[0].Kind == Gui::ElementKind::Panel);
    CHECK(elements[0].ChildCount == 5);
    CHECK(elements[1].Kind == Gui::ElementKind::Text);
    CHECK(elements[1].Id == "title");
    CHECK(elements[1].Text == "Status");
    CHECK(elements[2].Kind == Gui::ElementKind::Text);
    CHECK(elements[3].Kind == Gui::ElementKind::ProgressBar);
    CHECK(elements[4].Kind == Gui::ElementKind::Slider);
    CHECK(elements[5].Kind == Gui::ElementKind::Button);

    // The class tags survive.
    REQUIRE(elements[0].Classes.size() == 1);
    CHECK(elements[0].Classes[0] == "hud");
    REQUIRE(elements[5].Classes.size() == 1);
    CHECK(elements[5].Classes[0] == "primary");

    // The Slider's widget-config attributes (min/max/step literals plus a {value} binding) all store
    // as recipe bindings — the closed literal-attribute set the widget layer reads at instantiate.
    string sliderMin;
    string sliderMax;
    string sliderStep;
    string sliderValue;
    for (const Gui::UIBindingRecipe& binding : elements[4].Bindings)
    {
        if (binding.Property == "min")
        {
            sliderMin = binding.Expression;
        }
        if (binding.Property == "max")
        {
            sliderMax = binding.Expression;
        }
        if (binding.Property == "step")
        {
            sliderStep = binding.Expression;
        }
        if (binding.Property == "value")
        {
            sliderValue = binding.Expression;
        }
    }
    CHECK(sliderMin == "0");
    CHECK(sliderMax == "10");
    CHECK(sliderStep == "1");
    CHECK(sliderValue == "player.volume");

    // The {binding} attributes are stored unresolved on the ProgressBar.
    REQUIRE(elements[3].Bindings.size() == 2);
    bool sawValue = false;
    bool sawMax = false;
    for (const Gui::UIBindingRecipe& binding : elements[3].Bindings)
    {
        if (binding.Property == "value")
        {
            sawValue = true;
            CHECK(binding.Expression == "player.health");
        }
        if (binding.Property == "max")
        {
            sawMax = true;
            CHECK(binding.Expression == "player.healthMax");
        }
    }
    CHECK(sawValue);
    CHECK(sawMax);

    // The onClick handler is stored unresolved on the Button.
    REQUIRE(elements[5].Handlers.size() == 1);
    CHECK(elements[5].Handlers[0].Event == "onClick");
    CHECK(elements[5].Handlers[0].Handler == "OpenMenu");

    // (c) The document referenced exactly one stylesheet, resolved live.
    REQUIRE(recipe.GetStyleSheets().size() == 1);
    const AssetHandle<Gui::StyleSheet>& sheetHandle = recipe.GetStyleSheets()[0];
    CHECK(sheetHandle.Id().Value == StyleSheetId.Value);
    REQUIRE(sheetHandle.IsLoaded());

    // The inline `font: 0x…` on the title element pulled the font as a dependency (resolvable now).
    const AssetResult<AssetHandle<Font>> fontHandle = assets.LoadSync<Font>(FontId);
    REQUIRE(fontHandle.has_value());
    CHECK(fontHandle->IsLoaded());

    // (b) The stylesheet's resolved rules carry the base + state variants with linear colors.
    const Gui::StyleSheet& sheet = *sheetHandle.Get();

    const Gui::StyleRule* hud = FindRule(sheet, "", "hud", "", Gui::ElementState::None);
    REQUIRE(hud != nullptr);
    CHECK(FindDecl(*hud, Gui::StyleProperty::FlexDirection) != nullptr);
    CHECK(FindDecl(*hud, Gui::StyleProperty::Background) != nullptr);

    const Gui::StyleRule* primary = FindRule(sheet, "", "primary", "", Gui::ElementState::None);
    REQUIRE(primary != nullptr);
    const Gui::StyleDeclaration* bg = FindDecl(*primary, Gui::StyleProperty::Background);
    REQUIRE(bg != nullptr);
    // #3b82f6 sRGB → linear: the blue channel (0xf6/255 ≈ 0.965 sRGB) is well above the red.
    CHECK(bg->Values.b > bg->Values.r);
    CHECK(bg->Values.b <= 1.0f);

    const Gui::StyleRule* primaryHover =
        FindRule(sheet, "", "primary", "", Gui::ElementState::Hovered);
    REQUIRE(primaryHover != nullptr);
    CHECK(FindDecl(*primaryHover, Gui::StyleProperty::Background) != nullptr);

    const Gui::StyleRule* primaryDisabled =
        FindRule(sheet, "", "primary", "", Gui::ElementState::Disabled);
    REQUIRE(primaryDisabled != nullptr);
    const Gui::StyleDeclaration* opacity = FindDecl(*primaryDisabled, Gui::StyleProperty::Opacity);
    REQUIRE(opacity != nullptr);
    CHECK(opacity->Values.x == doctest::Approx(0.5f));

    // The Text#title id selector matched a typed rule.
    const Gui::StyleRule* title = FindRule(sheet, "Text", "", "title", Gui::ElementState::None);
    REQUIRE(title != nullptr);
    CHECK(FindDecl(*title, Gui::StyleProperty::TextSize) != nullptr);

    // The .tag rule round-trips the per-edge insets, pointer-events, and the animation
    // reference; the @keyframes clip round-trips as an indexed animation table.
    const Gui::StyleRule* tag = FindRule(sheet, "", "tag", "", Gui::ElementState::None);
    REQUIRE(tag != nullptr);
    const Gui::StyleDeclaration* insetRight = FindDecl(*tag, Gui::StyleProperty::InsetRight);
    REQUIRE(insetRight != nullptr);
    CHECK(insetRight->Values.x == doctest::Approx(12.0f));
    const Gui::StyleDeclaration* pointer = FindDecl(*tag, Gui::StyleProperty::PointerEvents);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->Unit == static_cast<u32>(Gui::PointerEvents::None));
    const Gui::StyleDeclaration* origin = FindDecl(*tag, Gui::StyleProperty::Origin);
    REQUIRE(origin != nullptr);
    CHECK(origin->Values.x == doctest::Approx(0.5f));
    CHECK(origin->Values.y == doctest::Approx(0.5f));
    const Gui::StyleDeclaration* animationRef = FindDecl(*tag, Gui::StyleProperty::Animation);
    REQUIRE(animationRef != nullptr);
    CHECK(animationRef->Values.x == doctest::Approx(2.0f));
    CHECK(animationRef->Values.y ==
          doctest::Approx(static_cast<f32>(Gui::AnimationLoopMode::PingPong)));
    REQUIRE(sheet.GetAnimations().size() == 1);
    REQUIRE(animationRef->Unit == 0);
    const Gui::StyleAnimationClip& clip = sheet.GetAnimations()[0];
    REQUIRE(clip.Keyframes.size() == 3);
    CHECK(clip.Keyframes[0].Offset == doctest::Approx(0.0f));
    CHECK(clip.Keyframes[1].Offset == doctest::Approx(0.5f));
    CHECK(clip.Keyframes[2].Offset == doctest::Approx(1.0f));
    REQUIRE(clip.Keyframes[1].Declarations.size() == 1);
    CHECK(clip.Keyframes[1].Declarations[0].Property == Gui::StyleProperty::Opacity);
    CHECK(clip.Keyframes[1].Declarations[0].Values.x == doctest::Approx(1.0f));

    // The `.hud` rule's `background-gradient` baked one gradient into the sheet's table: a vertical
    // (180deg) linear ramp from red to blue. The box-fit endpoints run top (P0) to bottom (P1), and
    // the multi-stop color is baked into the ramp — red at t=0, blue at t=1.
    REQUIRE(sheet.GetGradients().size() == 1);
    const Gui::StyleGradient& gradient = sheet.GetGradients()[0];
    CHECK(gradient.Kind == Gui::GradientKind::Linear);
    CHECK(gradient.P0.x == doctest::Approx(0.0f));
    CHECK(gradient.P0.y == doctest::Approx(-1.0f));
    CHECK(gradient.P1.x == doctest::Approx(0.0f));
    CHECK(gradient.P1.y == doctest::Approx(1.0f));
    REQUIRE(gradient.Width == 256);
    REQUIRE(gradient.Ramp.size() == 256u * 4u);
    // First texel red, last texel blue (linear straight-alpha RGBA8).
    CHECK(gradient.Ramp[0] > 250);
    CHECK(gradient.Ramp[1] < 5);
    CHECK(gradient.Ramp[2] < 5);
    CHECK(gradient.Ramp[255 * 4 + 0] < 5);
    CHECK(gradient.Ramp[255 * 4 + 2] > 250);

    // (d) Two instantiations are independent trees over one recipe.
    Unique<Gui::Document> a = Gui::Document::Instantiate(recipe, assets);
    Unique<Gui::Document> b = Gui::Document::Instantiate(recipe, assets);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // The gradient resolved onto the `.hud` root: its ramp uploaded to a resident texture through
    // the borrowed AssetManager, ready for the paint path to sample.
    REQUIRE(a->Root().ComputedStyle.BackgroundGradient.has_value());
    CHECK(a->Root().ComputedStyle.BackgroundGradient->Kind == Gui::GradientKind::Linear);
    CHECK(a->Root().ComputedStyle.BackgroundGradient->Ramp.IsLoaded());

    // The instantiated tree mirrors the markup: a Panel root with five children.
    CHECK(a->Root().Children.size() == 5);
    CHECK(b->Root().Children.size() == 5);
    CHECK(a->Root().Children[0]->Kind == Gui::ElementKind::Text);
    CHECK(a->Root().Children[0]->Id == "title");
    CHECK(a->Root().Children[0]->Text == "Status");
    CHECK(a->Root().Children[4]->Kind == Gui::ElementKind::Button);

    // The Slider instance parsed its min/max/step literals into its widget state, and is focusable.
    const Gui::Element& sliderInstance = *a->Root().Children[3];
    CHECK(sliderInstance.Kind == Gui::ElementKind::Slider);
    CHECK(sliderInstance.Focusable);
    CHECK(sliderInstance.Widget.Min == doctest::Approx(0.0f));
    CHECK(sliderInstance.Widget.Max == doctest::Approx(10.0f));
    CHECK(sliderInstance.Widget.Step == doctest::Approx(1.0f));

    // The inline `padding: 12px` on the root materialized onto the instance's resolved style.
    CHECK(a->Root().ComputedStyle.Padding.Left == doctest::Approx(12.0f));

    // Mutating one instance leaves the other untouched.
    a->SetText(*a->Root().Children[0], "Changed");
    CHECK(a->Root().Children[0]->Text == "Changed");
    CHECK(b->Root().Children[0]->Text == "Status");

    std::filesystem::remove(outArchive);
}

TEST_CASE("ui document: a truncated cooked blob decodes as Corrupt")
{
    // A blob shorter than the header, and one with a good header but a truncated body, both surface
    // as a recoverable AssetError::Corrupt rather than a crash — the untrusted-first decode policy.
    const vector<u8> tooShort(4, 0);
    const AssetResult<Detail::DecodedUIDocument> shortDoc =
        Detail::DecodeUIDocument(UIDocumentId, tooShort);
    REQUIRE_FALSE(shortDoc.has_value());
    CHECK(shortDoc.error().Kind == AssetError::Corrupt);

    const AssetResult<Detail::DecodedStyleSheet> shortSheet =
        Detail::DecodeStyleSheet(StyleSheetId, tooShort);
    REQUIRE_FALSE(shortSheet.has_value());
    CHECK(shortSheet.error().Kind == AssetError::Corrupt);

    // A valid-looking header claiming more elements than the blob carries is truncated.
    CookedUIDocumentHeader header{};
    header.Version = CookedUIDocumentVersion;
    header.ElementCount = 8;
    vector<u8> truncated(sizeof(header));
    std::memcpy(truncated.data(), &header, sizeof(header));
    const AssetResult<Detail::DecodedUIDocument> truncDoc =
        Detail::DecodeUIDocument(UIDocumentId, truncated);
    REQUIRE_FALSE(truncDoc.has_value());
    CHECK(truncDoc.error().Kind == AssetError::Corrupt);
}
