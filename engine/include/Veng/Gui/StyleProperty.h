#pragma once

#include <Veng/Veng.h>

#include <string_view>

namespace Veng::Gui
{
    /// @brief The closed set of style properties a USS declaration or inline style can set.
    ///
    /// One enumerator per settable Style field. A cooked style declaration (CookedStyleProperty)
    /// names its target field by this id, and the runtime applies the declaration's value onto the
    /// matching Style field. The enum is the shared vocabulary the cooker's USS parser produces and
    /// the runtime's style application consumes; its underlying integer is what the cooked blob
    /// stores. It is append-only — a new property is added at the end so an older cooked blob keeps
    /// decoding.
    ///
    /// Multi-field CSS properties are split to one enumerator per numeric payload: a border is
    /// BorderWidth (a scalar) plus BorderColor (a vec4), so every declaration fits the uniform
    /// CookedStyleProperty payload.
    ///
    /// A color property's value is a linear-space vec4. It authors two ways: hex `#rrggbb`/`#rrggbbaa`
    /// is an sRGB hue decoded to a linear color clamped to [0, 1] (the familiar LDR path), while
    /// `rgb(x, y, z)` / `rgba(x, y, z, a)` are unclamped linear floats taken directly (no sRGB decode),
    /// so a component may exceed 1 to author an emissive color that glows on a world surface. So
    /// `rgb(0.5, 0.5, 0.5)` is a linear 0.5 grey, distinct from `#808080`, which decodes to ~0.216
    /// linear.
    enum class StyleProperty : u32
    {
        /// @brief The flex main-axis direction (Style::Direction); value is a FlexDirection ordinal.
        FlexDirection = 0,
        /// @brief The main-axis distribution of children (Style::JustifyContent); value is a Justify ordinal.
        JustifyContent,
        /// @brief The cross-axis alignment of children (Style::AlignItems); value is an Align ordinal.
        AlignItems,
        /// @brief The element's own cross-axis alignment (Style::AlignSelf); value is an Align ordinal.
        AlignSelf,
        /// @brief Whether children wrap onto multiple lines (Style::Wrap); value is a FlexWrap ordinal.
        FlexWrap,
        /// @brief The element's grow factor (Style::FlexGrow); value is a scalar.
        FlexGrow,
        /// @brief The element's shrink factor (Style::FlexShrink); value is a scalar.
        FlexShrink,
        /// @brief The element's initial main-axis size (Style::FlexBasis); value is a Length.
        FlexBasis,
        /// @brief The element's width (Style::Width); value is a Length.
        Width,
        /// @brief The element's height (Style::Height); value is a Length.
        Height,
        /// @brief The element's minimum width (Style::MinWidth); value is a Length.
        MinWidth,
        /// @brief The element's minimum height (Style::MinHeight); value is a Length.
        MinHeight,
        /// @brief The element's maximum width (Style::MaxWidth); value is a Length.
        MaxWidth,
        /// @brief The element's maximum height (Style::MaxHeight); value is a Length.
        MaxHeight,
        /// @brief The element's outer margin (Style::Margin); value is four edge lengths (L/T/R/B).
        Margin,
        /// @brief The element's inner padding (Style::Padding); value is four edge lengths (L/T/R/B).
        Padding,
        /// @brief Whether the element flows normally or is absolutely positioned (Style::Position); value is a PositionType ordinal.
        Position,
        /// @brief The element's absolute insets (Style::Inset); value is four edge lengths (L/T/R/B).
        Inset,
        /// @brief The element's background fill (Style::Background); value is a linear-space vec4.
        Background,
        /// @brief The element's corner radius (Style::Radii); value is four corner radii (TL/TR/BR/BL).
        CornerRadius,
        /// @brief The element's border width (Style::BorderStyle.Width); value is a scalar.
        BorderWidth,
        /// @brief The element's border color (Style::BorderStyle.Color); value is a linear-space vec4.
        BorderColor,
        /// @brief The element's text fill (Style::TextColor); value is a linear-space vec4.
        TextColor,
        /// @brief The element's text em size in pixels (Style::TextSize); value is a scalar.
        TextSize,
        /// @brief The element's text font (Style::TextFont); value is a Font AssetId.
        TextFont,
        /// @brief The element's opacity multiplier (Style::Opacity); value is a scalar.
        Opacity,
        /// @brief Both overflow axes at once (Style::OverflowX and OverflowY); Values x/y are Overflow ordinals.
        ///
        /// The shorthand form, standing to OverflowX/OverflowY as Inset does to InsetLeft/InsetTop/…
        Overflow,
        /// @brief The horizontal overflow axis alone (Style::OverflowX); Unit is an Overflow ordinal.
        OverflowX,
        /// @brief The vertical overflow axis alone (Style::OverflowY); Unit is an Overflow ordinal.
        OverflowY,
        /// @brief Whether scrollbars overlay content or reserve a gutter (Style::Scrollbar); Unit is a ScrollbarLayout ordinal.
        ScrollbarLayout,
        /// @brief The absolute left inset alone (Style::Inset.Left); value is a scalar.
        InsetLeft,
        /// @brief The absolute top inset alone (Style::Inset.Top); value is a scalar.
        InsetTop,
        /// @brief The absolute right inset alone (Style::Inset.Right); value is a scalar.
        InsetRight,
        /// @brief The absolute bottom inset alone (Style::Inset.Bottom); value is a scalar.
        InsetBottom,
        /// @brief Whether the element hit-tests (Style::Pointer); value is a PointerEvents ordinal.
        PointerEvents,
        /// @brief A stylesheet animation reference (not a Style field): Unit is the sheet's clip
        /// index, Values are the duration in seconds (x) and an AnimationLoopMode ordinal (y).
        Animation,
        /// @brief The element's normalized self-anchor (Style::Origin); value is a vec2.
        Origin,
        /// @brief A gradient background fill (Style::BackgroundGradient), not a plain color: Unit is
        /// the sheet's gradient-table index the resolve materializes into a ResolvedGradient.
        BackgroundGradient,
        /// @brief The element's paint rotation in degrees (Style::Rotation); value is a scalar.
        Rotation,
        /// @brief A Text element's horizontal glyph alignment (Style::TextAlignment); value is a
        /// TextAlign ordinal.
        TextAlign,
        /// @brief A texture background fill (Style::BackgroundImage); value is a Texture AssetId.
        BackgroundImage,
        /// @brief The background image's nine-slice margins (Style::BackgroundSlice); value is four
        /// edge distances (L/T/R/B) in source-texture pixels.
        BackgroundSlice,
        /// @brief How the background image maps into its box (Style::BackgroundFit); Unit is an
        /// ImageFit ordinal.
        BackgroundFit,
        /// @brief Whether the background image stretches or tiles (Style::BackgroundRepeat); Unit is
        /// an ImageRepeat ordinal.
        BackgroundRepeat,
        /// @brief How an Image maps its texture into its content box (Style::ObjectFit); Unit is an
        /// ImageFit ordinal.
        ObjectFit,
        /// @brief Whether an Image stretches or tiles (Style::ImageRepeatMode); Unit is an
        /// ImageRepeat ordinal.
        ImageRepeat,
        /// @brief An Image's nine-slice margins (Style::ImageSlice); value is four edge distances
        /// (L/T/R/B) in source-texture pixels.
        ImageSlice,
        /// @brief The shadow's geometry and kind (Style::Shadow): Values are the x/y offset, the
        /// blur, and the spread in pixels; Unit is 0 for no shadow, 1 for a drop shadow, 2 for an
        /// inset one. Paired with BoxShadowColor as BorderWidth is with BorderColor.
        BoxShadow,
        /// @brief The shadow's color (Style::Shadow->Color); value is a linear-space vec4.
        BoxShadowColor,
        /// @brief A material background fill (Style::BackgroundMaterial); value is a Material or
        /// MaterialInstance AssetId.
        BackgroundMaterial,
        /// @brief The material an Image shades its content box through (Style::ImageMaterial);
        /// value is a Material or MaterialInstance AssetId.
        ImageMaterial,
        /// @brief A per-property ease list (not a Style field): Unit is the sheet's transition-table
        /// index the declaration's entries start at, Values.x is how many entries it holds.
        Transition,
    };

    /// @brief The kind of shadow a BoxShadow declaration's Unit selects.
    enum class BoxShadowMode : u32
    {
        /// @brief No shadow: the declaration clears Style::Shadow (`box-shadow: none`).
        None = 0,
        /// @brief A drop shadow, painted behind the element's fill.
        Drop = 1,
        /// @brief An inset shadow, painted over the element's fill.
        Inset = 2,
    };

    /// @brief The number of StyleProperty enumerators — keep in step when appending one.
    ///
    /// The runtime's whole-style property sweeps iterate `[0, StylePropertyCount)`.
    inline constexpr u32 StylePropertyCount = static_cast<u32>(StyleProperty::Transition) + 1;

    /// @brief Canonical USS declaration name of a style property ("flex-direction", "background", …).
    ///
    /// The single spelling the `*.vuss` / inline-style authoring agrees on. The inverse of
    /// @ref ParseStyleProperty. An unmapped value returns "unknown".
    /// @param property  The style property.
    /// @return A static, never-null name string.
    [[nodiscard]] const char* ToString(StyleProperty property);

    /// @brief Parses a USS declaration name into a StyleProperty.
    ///
    /// The inverse of @ref ToString; the one place a USS declaration name is decoded, used by the
    /// cooker's stylesheet and inline-style parsing.
    /// @param name  The declaration name (e.g. "background", "corner-radius").
    /// @return The matching property, or nullopt when the name is unrecognized.
    [[nodiscard]] optional<StyleProperty> ParseStyleProperty(std::string_view name);

    /// @brief Whether a property's value interpolates continuously, so it can be eased.
    ///
    /// True for colors, scalars, corner radii, edge insets, and same-kind lengths; false for the
    /// enums, fonts, fill-source references, and the shadow shorthand, whose values have no
    /// midpoint. This is the transition-able set: an entry naming a property outside it never
    /// tweens, so the style tween clock skips it and the stylesheet cook rejects it up front
    /// rather than letting an author discover it by watching a property snap.
    /// @param property  The style property.
    /// @return True when a transition or a keyframe clip can interpolate the property.
    [[nodiscard]] bool IsAnimatableProperty(StyleProperty property);
}
