#include <Veng/Gui/StyleSheet.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>

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

        // Reads a declaration's four-edge payload into an Insets. The cooked components run in
        // the CSS edge order — top, right, bottom, left — as xyzw.
        Insets InsetsFrom(const StyleDeclaration& declaration)
        {
            return Insets{.Left = declaration.Values.w,
                          .Top = declaration.Values.x,
                          .Right = declaration.Values.y,
                          .Bottom = declaration.Values.z};
        }

        // Reads a declaration's four-edge payload into a PositionInsets, all edges set. The
        // cooked components run in the CSS edge order — top, right, bottom, left — as xyzw.
        PositionInsets PositionInsetsFrom(const StyleDeclaration& declaration)
        {
            return PositionInsets{.Left = declaration.Values.w,
                                  .Top = declaration.Values.x,
                                  .Right = declaration.Values.y,
                                  .Bottom = declaration.Values.z};
        }
    }

    void ApplyDeclaration(Style& style, const StyleDeclaration& declaration, AssetManager* assets)
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
            if (assets != nullptr && declaration.Handle.IsValid())
            {
                AssetHandle<Font> font =
                    assets->LoadSync<Font>(declaration.Handle).value_or(AssetHandle<Font>{});
                if (font.Id().IsValid())
                {
                    style.TextFont = std::move(font);
                }
            }
            return;
        case StyleProperty::BackgroundImage:
            // The texture is already resident as a load-time dependency of the sheet (or of the
            // document, for an inline style), so this is a cache lookup. A miss leaves the fill
            // unset and the flat/gradient background paints instead — a missing font's tolerance.
            if (assets != nullptr && declaration.Handle.IsValid())
            {
                AssetHandle<Texture> texture =
                    assets->LoadSync<Texture>(declaration.Handle).value_or(AssetHandle<Texture>{});
                if (texture.Id().IsValid())
                {
                    style.BackgroundImage = std::move(texture);
                }
            }
            return;
        case StyleProperty::BackgroundMaterial:
        case StyleProperty::ImageMaterial:
        {
            // The material is already resident as a load-time dependency of the sheet (or of the
            // document, for an inline style), so this is a cache lookup. The id may name a
            // MaterialInstance or a bare Material — the instance loader resolves the latter to a
            // zero-override default instance, so both spellings author identically.
            if (assets == nullptr || !declaration.Handle.IsValid())
            {
                return;
            }
            AssetHandle<MaterialInstance> material =
                assets->LoadSync<MaterialInstance>(declaration.Handle)
                    .value_or(AssetHandle<MaterialInstance>{});
            if (!material.Id().IsValid())
            {
                return;
            }
            if (declaration.Property == StyleProperty::BackgroundMaterial)
            {
                style.BackgroundMaterial = std::move(material);
            }
            else
            {
                style.ImageMaterial = std::move(material);
            }
            return;
        }
        case StyleProperty::BackgroundSlice:
            style.BackgroundSlice = InsetsFrom(declaration);
            return;
        case StyleProperty::BackgroundFit:
            style.BackgroundFit = static_cast<ImageFit>(declaration.Unit);
            return;
        case StyleProperty::BackgroundRepeat:
            style.BackgroundRepeat = static_cast<ImageRepeat>(declaration.Unit);
            return;
        case StyleProperty::ObjectFit:
            style.ObjectFit = static_cast<ImageFit>(declaration.Unit);
            return;
        case StyleProperty::ImageRepeat:
            style.ImageRepeatMode = static_cast<ImageRepeat>(declaration.Unit);
            return;
        case StyleProperty::ImageSlice:
            style.ImageSlice = InsetsFrom(declaration);
            return;
        case StyleProperty::BoxShadow:
        {
            if (static_cast<BoxShadowMode>(declaration.Unit) == BoxShadowMode::None)
            {
                style.Shadow.reset();
                return;
            }
            // The geometry and the color are separate declarations, so each writes only its own
            // half of an existing shadow — a variant may restyle one without restating the other.
            BoxShadow shadow = style.Shadow.value_or(BoxShadow{});
            shadow.Offset = vec2(declaration.Values.x, declaration.Values.y);
            shadow.Blur = declaration.Values.z;
            shadow.Spread = declaration.Values.w;
            shadow.Inset = static_cast<BoxShadowMode>(declaration.Unit) == BoxShadowMode::Inset;
            style.Shadow = shadow;
            return;
        }
        case StyleProperty::BoxShadowColor:
        {
            BoxShadow shadow = style.Shadow.value_or(BoxShadow{});
            shadow.Color = declaration.Values;
            style.Shadow = shadow;
            return;
        }
        case StyleProperty::Opacity:
            style.Opacity = declaration.Values.x;
            return;
        case StyleProperty::Rotation:
            style.Rotation = declaration.Values.x;
            return;
        case StyleProperty::TextAlign:
            style.TextAlignment = static_cast<TextAlign>(declaration.Unit);
            return;
        case StyleProperty::TextWrap:
            style.Wrapping = static_cast<TextWrap>(declaration.Unit);
            return;
        case StyleProperty::Overflow:
            // The shorthand carries both axes; the longhands below each carry one in Unit.
            style.OverflowX = static_cast<Overflow>(static_cast<i32>(declaration.Values.x));
            style.OverflowY = static_cast<Overflow>(static_cast<i32>(declaration.Values.y));
            return;
        case StyleProperty::OverflowX:
            style.OverflowX = static_cast<Overflow>(declaration.Unit);
            return;
        case StyleProperty::OverflowY:
            style.OverflowY = static_cast<Overflow>(declaration.Unit);
            return;
        case StyleProperty::ScrollbarLayout:
            style.Scrollbar = static_cast<ScrollbarLayout>(declaration.Unit);
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
        case StyleProperty::BackgroundGradient:
            // A gradient references the sheet's gradient table and needs the borrowed AssetManager to
            // upload its ramp, so the instantiate-time cascade resolves it where the sheet is in hand
            // (ResolveElementStyle); it is not resolvable from a declaration alone here.
            return;
        case StyleProperty::Transition:
            // An ease list is element state, not a Style field; it slices the sheet's transition
            // table, so the instantiate-time style resolve routes it onto the element.
            return;
        }
    }

    Ref<StyleSheet> StyleSheet::Create(vector<StyleRule> rules,
                                       vector<StyleAnimationClip> animations,
                                       vector<StyleGradient> gradients,
                                       vector<StyleVariable> variables,
                                       vector<StyleTransition> transitions,
                                       vector<Ref<Detail::AssetCacheEntry>> dependencies)
    {
        return Ref<StyleSheet>(new StyleSheet(std::move(rules), std::move(animations),
                                              std::move(gradients), std::move(variables),
                                              std::move(transitions), std::move(dependencies)));
    }

    StyleSheet::StyleSheet(vector<StyleRule> rules, vector<StyleAnimationClip> animations,
                           vector<StyleGradient> gradients, vector<StyleVariable> variables,
                           vector<StyleTransition> transitions,
                           vector<Ref<Detail::AssetCacheEntry>> dependencies)
        : m_Rules(std::move(rules)), m_Animations(std::move(animations)),
          m_Gradients(std::move(gradients)), m_Variables(std::move(variables)),
          m_Transitions(std::move(transitions)), m_Dependencies(std::move(dependencies))
    {
    }

    optional<vec4> StyleSheet::FindVariableColor(string_view name) const
    {
        for (const StyleVariable& variable : m_Variables)
        {
            if (variable.Kind == StyleVariableKind::Color && variable.Name == name)
            {
                return variable.Payload;
            }
        }
        return std::nullopt;
    }

    optional<f32> StyleSheet::FindVariableScalar(string_view name) const
    {
        for (const StyleVariable& variable : m_Variables)
        {
            if (variable.Kind == StyleVariableKind::Scalar && variable.Name == name)
            {
                return variable.Payload.x;
            }
        }
        return std::nullopt;
    }
}
