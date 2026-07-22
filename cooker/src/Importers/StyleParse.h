#pragma once

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Gui/StyleProperty.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

#include <string_view>

namespace Veng::Cook
{
    /// @brief Parses one USS declaration value into a CookedStyleProperty.
    ///
    /// The single value-parsing path both the stylesheet importer (a `*.vuss` block declaration)
    /// and the UI-document importer (an inline `style="..."` declaration) share, so a value is read
    /// identically wherever it is authored. It interprets `value` per `property`: a length
    /// (`12px` / `50%` / `auto`), a color (`#rrggbb` / `#rrggbbaa` resolved sRGB → linear, or
    /// `rgb(x,y,z)` / `rgba(x,y,z,a)` taken as unclamped linear floats), a scalar, a boolean/overflow
    /// keyword, one of the flex enums by its USS keyword, a
    /// four-edge shorthand (margin/padding/inset/corner-radius, 1–4 space-separated lengths), or a
    /// font AssetId (`0x…`). A malformed value is a located error carrying `located` as its prefix.
    /// @param property  The property the declaration sets.
    /// @param value     The declaration's value text (trimmed of surrounding whitespace).
    /// @param located   The located-error prefix (file + selector/element context).
    /// @return The cooked declaration, or a located error.
    [[nodiscard]] Result<CookedStyleProperty> ParseStyleDeclaration(Gui::StyleProperty property,
                                                                    std::string_view value,
                                                                    const string& located);

    /// @brief Parses a `box-shadow` shorthand into its geometry and color declarations.
    ///
    /// The shadow is split across two cooked declarations the way a border is —
    /// `StyleProperty::BoxShadow` carrying the offset, blur, and spread in its Values and the
    /// BoxShadowMode in its Unit, `StyleProperty::BoxShadowColor` carrying the color — so both fit
    /// the uniform CookedStyleProperty payload. The grammar is
    /// `<offx> <offy> [blur] [spread] [color] [inset]` (the `inset` keyword may lead or trail), or
    /// the bare keyword `none`, which cooks the geometry declaration alone with a None mode.
    /// @param value    The declaration's value text.
    /// @param located  The located-error prefix (file + selector/element context).
    /// @return The one or two cooked declarations, or a located error.
    [[nodiscard]] Result<vector<CookedStyleProperty>>
    ParseBoxShadowDeclaration(std::string_view value, const string& located);

    /// @brief Parses a color value (`#rrggbb`/`#rrggbbaa` hex or `rgb()`/`rgba()`) to linear.
    ///
    /// The shared color parse both a `background`/`color` declaration and a gradient stop resolve
    /// through, so a color reads identically wherever it is authored. A hex color's RGB channels
    /// convert sRGB → linear (the draw-list contract) and its alpha stays a straight [0, 1] value.
    /// An `rgb(x, y, z)` / `rgba(x, y, z, a)` color is taken as unclamped linear floats (each >= 0,
    /// a value > 1 authoring an emissive/HDR color) with no sRGB decode; `rgb()` defaults alpha to 1.
    /// @param value    The color text (hex with or without a leading `#`, or an rgb()/rgba() call).
    /// @param located  The located-error prefix (file + selector/element context).
    /// @return The linear straight-alpha color, or a located error.
    [[nodiscard]] Result<vec4> ParseStyleColor(std::string_view value, const string& located);

    /// @brief Rejects a declaration block that authors more than one background fill source.
    ///
    /// Fill sources are exclusive at paint — `background-gradient` beats `background-image` beats
    /// `background`, and the winner *is* the fill — so a block naming two says something the
    /// renderer cannot honour. Catching it at cook turns a silently-ignored declaration into a
    /// located error. Run over a finished rule block or inline style, whose declarations are all
    /// one element's.
    /// @param properties  The block's cooked declarations.
    /// @param located     The located-error prefix (file + selector/element context).
    /// @return Nothing, or a located error naming the two conflicting sources.
    [[nodiscard]] VoidResult
    CheckExclusiveFillSources(const vector<CookedStyleProperty>& properties, const string& located);
}
