#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Gui/DrawList.h>

namespace Veng
{
    class Font;
}

/// @brief The resolved per-element style: flex layout inputs plus paint inputs.
///
/// A Style is a single flat set of values with no cascade, selectors, or state variants —
/// the fully resolved appearance and layout of one element. The layout inputs (flex
/// direction, grow/shrink/basis, alignment, wrap, sizing, spacing, absolute position) feed
/// the flexbox solver; the paint inputs (background, border, corner radii, text color/size/
/// font, opacity) feed the draw list. All spatial values are in framebuffer pixels.
namespace Veng::Gui
{
    /// @brief How a sizing value (width, height, flex basis) is expressed.
    enum class LengthKind : u8
    {
        /// @brief Sized automatically from content and the flex algorithm.
        Auto,
        /// @brief A fixed length in framebuffer pixels.
        Points,
        /// @brief A percentage of the containing block's corresponding axis.
        Percent,
    };

    /// @brief A sizing length: an Auto sentinel, a pixel value, or a percentage.
    ///
    /// The default is Auto (the value is ignored). Points carries a pixel length; Percent
    /// carries a 0..100 percentage of the parent's axis. The static builders are the
    /// intended construction path.
    struct Length
    {
        /// @brief Which interpretation Value carries.
        LengthKind Kind = LengthKind::Auto;
        /// @brief The pixel length (Points) or percentage (Percent); ignored when Auto.
        f32 Value = 0.0f;

        /// @brief Builds an Auto length (content-sized).
        /// @return A Length with Kind Auto.
        static Length Auto() { return {.Kind = LengthKind::Auto, .Value = 0.0f}; }

        /// @brief Builds a fixed pixel length.
        /// @param points  The length in framebuffer pixels.
        /// @return A Length with Kind Points.
        static Length Points(f32 points) { return {.Kind = LengthKind::Points, .Value = points}; }

        /// @brief Builds a percentage length of the parent's axis.
        /// @param percent  The percentage in 0..100.
        /// @return A Length with Kind Percent.
        static Length Percent(f32 percent)
        {
            return {.Kind = LengthKind::Percent, .Value = percent};
        }
    };

    /// @brief The main-axis direction children are laid out along.
    enum class FlexDirection : u8
    {
        /// @brief Left to right.
        Row,
        /// @brief Right to left.
        RowReverse,
        /// @brief Top to bottom.
        Column,
        /// @brief Bottom to top.
        ColumnReverse,
    };

    /// @brief How children are distributed along the main axis.
    enum class Justify : u8
    {
        /// @brief Packed toward the start.
        FlexStart,
        /// @brief Centered.
        Center,
        /// @brief Packed toward the end.
        FlexEnd,
        /// @brief Equal space between children, none at the edges.
        SpaceBetween,
        /// @brief Equal space around each child.
        SpaceAround,
        /// @brief Equal space between children and at the edges.
        SpaceEvenly,
    };

    /// @brief How children (Align::Items) or one child (Align::Self) sit on the cross axis.
    enum class Align : u8
    {
        /// @brief Inherit the parent's cross-axis alignment (Self) or default to Stretch (Items).
        Auto,
        /// @brief Packed toward the cross-axis start.
        FlexStart,
        /// @brief Centered on the cross axis.
        Center,
        /// @brief Packed toward the cross-axis end.
        FlexEnd,
        /// @brief Stretched to fill the cross axis.
        Stretch,
    };

    /// @brief Whether children wrap onto multiple lines when they overflow the main axis.
    enum class FlexWrap : u8
    {
        /// @brief Single line; children shrink to fit.
        NoWrap,
        /// @brief Wrap onto additional lines toward the cross-axis end.
        Wrap,
        /// @brief Wrap onto additional lines toward the cross-axis start.
        WrapReverse,
    };

    /// @brief Whether an element participates in normal flow or is absolutely positioned.
    enum class PositionType : u8
    {
        /// @brief Laid out in normal flex flow (the default).
        Relative,
        /// @brief Removed from flow and positioned by the Position insets against its parent.
        Absolute,
    };

    /// @brief The resolved style of one element: its flex layout inputs and its paint inputs.
    struct Style
    {
        /// @brief The main-axis direction children flow along.
        FlexDirection Direction = FlexDirection::Column;
        /// @brief Main-axis distribution of children.
        Justify JustifyContent = Justify::FlexStart;
        /// @brief Cross-axis alignment applied to every child.
        Align AlignItems = Align::Stretch;
        /// @brief Cross-axis alignment override for this element within its parent.
        Align AlignSelf = Align::Auto;
        /// @brief Whether children wrap onto multiple lines.
        FlexWrap Wrap = FlexWrap::NoWrap;

        /// @brief The element's grow factor: share of free main-axis space it absorbs.
        f32 FlexGrow = 0.0f;
        /// @brief The element's shrink factor: share of overflow it gives up.
        f32 FlexShrink = 1.0f;
        /// @brief The element's initial main-axis size before grow/shrink.
        Length FlexBasis = Length::Auto();

        /// @brief Fixed or content width.
        Length Width = Length::Auto();
        /// @brief Fixed or content height.
        Length Height = Length::Auto();
        /// @brief Lower bound on width; Auto for none.
        Length MinWidth = Length::Auto();
        /// @brief Lower bound on height; Auto for none.
        Length MinHeight = Length::Auto();
        /// @brief Upper bound on width; Auto for none.
        Length MaxWidth = Length::Auto();
        /// @brief Upper bound on height; Auto for none.
        Length MaxHeight = Length::Auto();

        /// @brief Outer spacing around the element's margin box, in pixels.
        Insets Margin;
        /// @brief Inner spacing between the border and the content box, in pixels.
        Insets Padding;

        /// @brief Whether the element flows normally or is absolutely positioned.
        PositionType Position = PositionType::Relative;
        /// @brief Absolute insets from the parent's edges, applied when Position is Absolute.
        Insets Inset;

        /// @brief Background fill color, linear straight-alpha RGBA; a zero alpha draws nothing.
        vec4 Background{0.0f};
        /// @brief Per-corner background/border radius, in pixels.
        CornerRadii Radii;
        /// @brief Border width and color; a zero width draws no border.
        Border BorderStyle;

        /// @brief Text fill color, linear straight-alpha RGBA (Text elements).
        vec4 TextColor{1.0f};
        /// @brief Text em size, in pixels (Text elements).
        f32 TextSize = 16.0f;
        /// @brief The font a Text element shapes and draws through; empty renders no text.
        AssetHandle<Font> TextFont;

        /// @brief Multiplier applied to the element's alpha, in 0..1.
        f32 Opacity = 1.0f;

        /// @brief Whether content outside the element's box is clipped to it.
        bool ClipContent = false;
    };
}
