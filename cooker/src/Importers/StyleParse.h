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
}
