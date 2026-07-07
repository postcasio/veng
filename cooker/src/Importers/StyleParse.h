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
    /// (`12px` / `50%` / `auto`), a color (`#rrggbb` / `#rrggbbaa`, resolved sRGB → linear straight
    /// alpha), a scalar, a boolean/overflow keyword, one of the flex enums by its USS keyword, a
    /// four-edge shorthand (margin/padding/inset/corner-radius, 1–4 space-separated lengths), or a
    /// font AssetId (`0x…`). A malformed value is a located error carrying `located` as its prefix.
    /// @param property  The property the declaration sets.
    /// @param value     The declaration's value text (trimmed of surrounding whitespace).
    /// @param located   The located-error prefix (file + selector/element context).
    /// @return The cooked declaration, or a located error.
    [[nodiscard]] Result<CookedStyleProperty> ParseStyleDeclaration(Gui::StyleProperty property,
                                                                    std::string_view value,
                                                                    const string& located);

    /// @brief Parses a CSS hex color (`#rrggbb` / `#rrggbbaa`, or the same without `#`) to linear.
    ///
    /// The shared color parse both a `background`/`color` declaration and a gradient stop resolve
    /// through, so a color reads identically wherever it is authored: the RGB channels convert
    /// sRGB → linear (the draw-list contract) and alpha stays a straight [0, 1] value.
    /// @param value    The color text, with or without a leading `#`.
    /// @param located  The located-error prefix (file + selector/element context).
    /// @return The linear straight-alpha color, or a located error.
    [[nodiscard]] Result<vec4> ParseStyleColor(std::string_view value, const string& located);
}
