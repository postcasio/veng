#pragma once

#include <limits>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Gui/DrawList.h>

namespace Veng
{
    class Font;
    class Texture;
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
    /// @brief A gradient background resolved onto a Style: its shape, geometry, and resident ramp.
    ///
    /// The cook bakes a gradient's multi-stop color into an N×1 ramp LUT and stores its shape +
    /// box-space geometry; the instantiate-time resolve uploads the ramp to a texture and keeps the
    /// handle here (resident for the Style's lifetime, like TextFont). The paint path copies the
    /// shape + geometry and the ramp's bindless handle/sampler into a Gui::GradientFill. Geometry is
    /// in the element's normalized box space and interpreted per Kind — see Gui::GradientFill. The
    /// fields are plain values, so a game can animate a gradient by mutating them per frame.
    struct ResolvedGradient
    {
        /// @brief The gradient shape (Linear / Radial / Conic).
        GradientKind Kind = GradientKind::Linear;
        /// @brief Linear start point / radial + conic center, in normalized box space.
        vec2 P0{0.0f};
        /// @brief Linear end point / radial (x, y) radii, in normalized box space.
        vec2 P1{0.0f};
        /// @brief Conic start turn in [0, 1); unused by the other kinds.
        f32 AngleOffset = 0.0f;
        /// @brief The resident N×1 ramp LUT (linear straight-alpha); its handle + sampler paint the fill.
        AssetHandle<Texture> Ramp;
    };

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

    /// @brief Whether an element (and its subtree) participates in pointer hit-testing.
    enum class PointerEvents : u8
    {
        /// @brief The element hit-tests normally (the default).
        Auto,
        /// @brief The element and its whole subtree are transparent to hit-testing.
        ///
        /// A display-only overlay piece (a cursor-following label) styled None never occludes
        /// the content underneath it: Document::HitTest and the pointer event path pass through.
        None,
    };

    /// @brief The absolute-position insets: per-edge offsets from the parent's edges, each optional.
    ///
    /// An edge holds a pixel offset or the Unset sentinel. Only set edges constrain the element:
    /// a Left-only pin places the element at that offset with its own (styled or content) size, a
    /// Right/Bottom pin anchors it to the far corner, and opposing set edges with an Auto size
    /// stretch the element between them. All edges default Unset, so an absolute element with no
    /// insets sits at its static position at content size.
    struct PositionInsets
    {
        /// @brief The sentinel an unconstrained edge holds.
        static constexpr f32 Unset = std::numeric_limits<f32>::infinity();

        /// @brief Offset from the parent's left edge, in pixels; Unset for unconstrained.
        f32 Left = Unset;
        /// @brief Offset from the parent's top edge, in pixels; Unset for unconstrained.
        f32 Top = Unset;
        /// @brief Offset from the parent's right edge, in pixels; Unset for unconstrained.
        f32 Right = Unset;
        /// @brief Offset from the parent's bottom edge, in pixels; Unset for unconstrained.
        f32 Bottom = Unset;

        /// @brief Returns whether an edge value constrains its edge (is not the Unset sentinel).
        /// @param edge  The edge value to test.
        [[nodiscard]] static bool IsSet(f32 edge) { return edge != Unset; }
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
        /// @brief Absolute per-edge insets, applied when Position is Absolute; unset edges free.
        PositionInsets Inset;
        /// @brief The element's self-anchor, as normalized fractions of its own laid-out size.
        ///
        /// After layout, the element (with its whole subtree) shifts by -Origin · size, so its
        /// position names where the anchor point sits rather than the top-left corner — an
        /// absolute element with `origin: 0.5 0.5` pins its center at its Left/Top insets, and a
        /// size change (a pulse animation) grows around the anchor instead of the corner. The
        /// default (0, 0) anchors the top-left, today's behavior. A pure post-layout offset: it
        /// never moves siblings or affects the flex solve.
        vec2 Origin{0.0f};

        /// @brief Background fill color, linear straight-alpha RGBA; a zero alpha draws nothing.
        vec4 Background{0.0f};
        /// @brief A gradient background fill; when set it paints instead of the flat Background color.
        optional<ResolvedGradient> BackgroundGradient;
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
        ///
        /// Composites over the subtree at draw: descendants multiply their ancestors'
        /// opacities into every primitive they emit, so fading an element fades its
        /// background, border, text, widget parts, and children as one.
        f32 Opacity = 1.0f;

        /// @brief Whether content outside the element's box is clipped to it.
        bool ClipContent = false;

        /// @brief Whether the element (and its subtree) takes part in pointer hit-testing.
        PointerEvents Pointer = PointerEvents::Auto;
    };
}
