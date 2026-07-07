#include <Veng/Gui/StyleSheet.h>

#include <Veng/Asset/Font.h>

namespace Veng::Gui
{
    namespace
    {
        // Reconstructs a Length from a declaration's Unit (the LengthKind ordinal) and value (Values.x).
        Length LengthFrom(const StyleDeclaration& declaration)
        {
            return Length{.Kind = static_cast<LengthKind>(declaration.Unit),
                          .Value = declaration.Values.x};
        }

        // Reads a declaration's four-edge payload (L/T/R/B) into an Insets.
        Insets InsetsFrom(const StyleDeclaration& declaration)
        {
            return Insets{.Left = declaration.Values.x,
                          .Top = declaration.Values.y,
                          .Right = declaration.Values.z,
                          .Bottom = declaration.Values.w};
        }

        // Reads a declaration's four-edge payload into a PositionInsets, all edges set.
        PositionInsets PositionInsetsFrom(const StyleDeclaration& declaration)
        {
            return PositionInsets{.Left = declaration.Values.x,
                                  .Top = declaration.Values.y,
                                  .Right = declaration.Values.z,
                                  .Bottom = declaration.Values.w};
        }
    }

    void ApplyDeclaration(Style& style, const StyleDeclaration& declaration,
                          const FontResolver& fonts)
    {
        switch (declaration.Property)
        {
        case StyleProperty::FlexDirection:
            style.Direction = static_cast<FlexDirection>(declaration.Unit);
            return;
        case StyleProperty::JustifyContent:
            style.JustifyContent = static_cast<Justify>(declaration.Unit);
            return;
        case StyleProperty::AlignItems:
            style.AlignItems = static_cast<Align>(declaration.Unit);
            return;
        case StyleProperty::AlignSelf:
            style.AlignSelf = static_cast<Align>(declaration.Unit);
            return;
        case StyleProperty::FlexWrap:
            style.Wrap = static_cast<FlexWrap>(declaration.Unit);
            return;
        case StyleProperty::FlexGrow:
            style.FlexGrow = declaration.Values.x;
            return;
        case StyleProperty::FlexShrink:
            style.FlexShrink = declaration.Values.x;
            return;
        case StyleProperty::FlexBasis:
            style.FlexBasis = LengthFrom(declaration);
            return;
        case StyleProperty::Width:
            style.Width = LengthFrom(declaration);
            return;
        case StyleProperty::Height:
            style.Height = LengthFrom(declaration);
            return;
        case StyleProperty::MinWidth:
            style.MinWidth = LengthFrom(declaration);
            return;
        case StyleProperty::MinHeight:
            style.MinHeight = LengthFrom(declaration);
            return;
        case StyleProperty::MaxWidth:
            style.MaxWidth = LengthFrom(declaration);
            return;
        case StyleProperty::MaxHeight:
            style.MaxHeight = LengthFrom(declaration);
            return;
        case StyleProperty::Margin:
            style.Margin = InsetsFrom(declaration);
            return;
        case StyleProperty::Padding:
            style.Padding = InsetsFrom(declaration);
            return;
        case StyleProperty::Position:
            style.Position = static_cast<PositionType>(declaration.Unit);
            return;
        case StyleProperty::Inset:
            style.Inset = PositionInsetsFrom(declaration);
            return;
        case StyleProperty::Background:
            style.Background = declaration.Values;
            return;
        case StyleProperty::CornerRadius:
            style.Radii = CornerRadii{.TopLeft = declaration.Values.x,
                                      .TopRight = declaration.Values.y,
                                      .BottomRight = declaration.Values.z,
                                      .BottomLeft = declaration.Values.w};
            return;
        case StyleProperty::BorderWidth:
            style.BorderStyle.Width = declaration.Values.x;
            return;
        case StyleProperty::BorderColor:
            style.BorderStyle.Color = declaration.Values;
            return;
        case StyleProperty::TextColor:
            style.TextColor = declaration.Values;
            return;
        case StyleProperty::TextSize:
            style.TextSize = declaration.Values.x;
            return;
        case StyleProperty::TextFont:
            if (fonts && declaration.Font.IsValid())
            {
                AssetHandle<Font> font = fonts(declaration.Font);
                if (font.Id().IsValid())
                {
                    style.TextFont = std::move(font);
                }
            }
            return;
        case StyleProperty::Opacity:
            style.Opacity = declaration.Values.x;
            return;
        case StyleProperty::ClipContent:
            style.ClipContent = declaration.Values.x != 0.0f;
            return;
        case StyleProperty::InsetLeft:
            style.Inset.Left = declaration.Values.x;
            return;
        case StyleProperty::InsetTop:
            style.Inset.Top = declaration.Values.x;
            return;
        case StyleProperty::InsetRight:
            style.Inset.Right = declaration.Values.x;
            return;
        case StyleProperty::InsetBottom:
            style.Inset.Bottom = declaration.Values.x;
            return;
        case StyleProperty::PointerEvents:
            style.Pointer = static_cast<PointerEvents>(declaration.Unit);
            return;
        case StyleProperty::Animation:
            // An animation reference is element state, not a Style field; the instantiate-time
            // style resolve routes it onto the element's animation list.
            return;
        case StyleProperty::Origin:
            style.Origin = vec2(declaration.Values.x, declaration.Values.y);
            return;
        }
    }

    Ref<StyleSheet> StyleSheet::Create(vector<StyleRule> rules,
                                       vector<StyleAnimationClip> animations,
                                       vector<Ref<Detail::AssetCacheEntry>> dependencies)
    {
        return Ref<StyleSheet>(
            new StyleSheet(std::move(rules), std::move(animations), std::move(dependencies)));
    }

    StyleSheet::StyleSheet(vector<StyleRule> rules, vector<StyleAnimationClip> animations,
                           vector<Ref<Detail::AssetCacheEntry>> dependencies)
        : m_Rules(std::move(rules)), m_Animations(std::move(animations)),
          m_Dependencies(std::move(dependencies))
    {
    }
}
