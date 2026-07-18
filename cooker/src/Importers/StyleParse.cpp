#include "StyleParse.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/HexId.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Style.h>

namespace Veng::Cook
{
    using Gui::StyleProperty;

    namespace
    {
        std::string_view Trim(std::string_view text)
        {
            usize begin = 0;
            usize end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
            {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
            {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        Result<f32> ParseFloat(std::string_view text, const string& located)
        {
            f32 value = 0.0f;
            const std::from_chars_result result =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{})
            {
                return std::unexpected(
                    fmt::format("{}: expected a number, got '{}'", located, text));
            }
            return value;
        }

        // A CSS hex color body (rrggbb or rrggbbaa) → a linear straight-alpha vec4. The RGB channels
        // convert sRGB → linear (the draw-list contract); alpha stays a straight [0,1] value.
        Result<vec4> ParseHexColor(std::string_view body, const string& located)
        {
            if (body.size() != 6 && body.size() != 8)
            {
                return std::unexpected(
                    fmt::format("{}: color '#{}' must be 6 (rrggbb) or 8 (rrggbbaa) hex digits",
                                located, body));
            }
            u32 channels[4] = {0, 0, 0, 255};
            const usize count = body.size() / 2;
            for (usize c = 0; c < count; ++c)
            {
                u32 v = 0;
                for (usize d = 0; d < 2; ++d)
                {
                    const char h = body[c * 2 + d];
                    u32 digit = 0;
                    if (h >= '0' && h <= '9')
                    {
                        digit = static_cast<u32>(h - '0');
                    }
                    else if (h >= 'a' && h <= 'f')
                    {
                        digit = static_cast<u32>(h - 'a') + 10;
                    }
                    else if (h >= 'A' && h <= 'F')
                    {
                        digit = static_cast<u32>(h - 'A') + 10;
                    }
                    else
                    {
                        return std::unexpected(fmt::format(
                            "{}: color '#{}' has a non-hex digit '{}'", located, body, h));
                    }
                    v = (v << 4) | digit;
                }
                channels[c] = v;
            }

            const auto toLinear = [](u32 srgb8) -> f32
            {
                const f32 c = static_cast<f32>(srgb8) / 255.0f;
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            return vec4(toLinear(channels[0]), toLinear(channels[1]), toLinear(channels[2]),
                        static_cast<f32>(channels[3]) / 255.0f);
        }

        // An rgb()/rgba() functional color → a linear straight-alpha vec4. Components are unclamped
        // linear floats where >= 0 (a value > 1 is an emissive/HDR color); no sRGB decode is applied,
        // so the parsed vec4 is the draw-list's linear color directly. rgb() takes three components
        // and defaults alpha to 1; rgba() takes four. `value` is trimmed and begins with "rgb".
        Result<vec4> ParseRgbFunction(std::string_view value, const string& located)
        {
            const bool hasAlpha = value.substr(0, 4) == "rgba";
            const usize prefix = hasAlpha ? 4 : 3;
            const usize expected = hasAlpha ? 4 : 3;
            const char* name = hasAlpha ? "rgba" : "rgb";
            const std::string_view rest = Trim(value.substr(prefix));
            if (rest.size() < 2 || rest.front() != '(' || rest.back() != ')')
            {
                return std::unexpected(
                    fmt::format("{}: color '{}' must be '{}(...)'", located, value, name));
            }
            const std::string_view inner = rest.substr(1, rest.size() - 2);

            f32 components[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            usize count = 0;
            usize begin = 0;
            for (usize i = 0; i <= inner.size(); ++i)
            {
                if (i != inner.size() && inner[i] != ',')
                {
                    continue;
                }
                if (count >= expected)
                {
                    return std::unexpected(fmt::format("{}: color '{}' takes {} components",
                                                       located, value, expected));
                }
                const std::string_view token = Trim(inner.substr(begin, i - begin));
                const Result<f32> component = ParseFloat(token, located);
                if (!component)
                {
                    return std::unexpected(component.error());
                }
                if (*component < 0.0f)
                {
                    return std::unexpected(fmt::format("{}: color '{}' component '{}' must be >= 0",
                                                       located, value, token));
                }
                components[count++] = *component;
                begin = i + 1;
            }
            if (count != expected)
            {
                return std::unexpected(fmt::format("{}: color '{}' takes {} components, got {}",
                                                   located, value, expected, count));
            }
            return vec4(components[0], components[1], components[2], components[3]);
        }

        // Resolves a color value to a linear straight-alpha vec4: an rgb()/rgba() functional color
        // (linear floats, HDR-capable) or a hex color (`#rrggbb`/`#rrggbbaa`, sRGB decoded to linear).
        Result<vec4> ParseColorValue(std::string_view value, const string& located)
        {
            const std::string_view trimmed = Trim(value);
            if (trimmed.substr(0, 3) == "rgb")
            {
                return ParseRgbFunction(trimmed, located);
            }
            std::string_view body = trimmed;
            if (!body.empty() && body.front() == '#')
            {
                body = body.substr(1);
            }
            return ParseHexColor(body, located);
        }

        // Parses one length token (auto / Npx / N% / a bare number treated as points) into the
        // (Unit, value) pair a CookedStyleProperty carries for a Length.
        Result<std::pair<u32, f32>> ParseLength(std::string_view text, const string& located)
        {
            const std::string_view t = Trim(text);
            if (t == "auto")
            {
                return std::pair<u32, f32>{static_cast<u32>(Gui::LengthKind::Auto), 0.0f};
            }
            if (!t.empty() && t.back() == '%')
            {
                const Result<f32> v = ParseFloat(t.substr(0, t.size() - 1), located);
                if (!v)
                {
                    return std::unexpected(v.error());
                }
                return std::pair<u32, f32>{static_cast<u32>(Gui::LengthKind::Percent), *v};
            }
            std::string_view number = t;
            if (t.size() >= 2 && t.substr(t.size() - 2) == "px")
            {
                number = t.substr(0, t.size() - 2);
            }
            const Result<f32> v = ParseFloat(number, located);
            if (!v)
            {
                return std::unexpected(v.error());
            }
            return std::pair<u32, f32>{static_cast<u32>(Gui::LengthKind::Points), *v};
        }

        // Parses a 1–4 length shorthand (margin/padding/inset/corner-radius) into four edge values.
        // One value applies to all four; the CSS shorthand order is honored for two/three/four.
        Result<vec4> ParseEdgeShorthand(std::string_view value, const string& located)
        {
            vector<f32> parts;
            usize i = 0;
            const usize n = value.size();
            while (i < n)
            {
                while (i < n && std::isspace(static_cast<unsigned char>(value[i])) != 0)
                {
                    ++i;
                }
                const usize start = i;
                while (i < n && std::isspace(static_cast<unsigned char>(value[i])) == 0)
                {
                    ++i;
                }
                if (i > start)
                {
                    const Result<std::pair<u32, f32>> len =
                        ParseLength(value.substr(start, i - start), located);
                    if (!len)
                    {
                        return std::unexpected(len.error());
                    }
                    parts.push_back(len->second);
                }
            }
            if (parts.empty() || parts.size() > 4)
            {
                return std::unexpected(fmt::format(
                    "{}: expected 1 to 4 space-separated lengths, got {}", located, parts.size()));
            }
            // The components stay in the order they are authored, which the runtime reads as the
            // CSS box order: top/right/bottom/left for the edge shorthands, and top-left,
            // top-right, bottom-right, bottom-left for corner-radius.
            switch (parts.size())
            {
            case 1:
                return vec4(parts[0]);
            case 2:
                return vec4(parts[0], parts[1], parts[0], parts[1]);
            case 3:
                return vec4(parts[0], parts[1], parts[2], parts[1]);
            default:
                return vec4(parts[0], parts[1], parts[2], parts[3]);
            }
        }

        optional<u32> ParseFlexDirection(std::string_view v)
        {
            if (v == "row")
            {
                return static_cast<u32>(Gui::FlexDirection::Row);
            }
            if (v == "row-reverse")
            {
                return static_cast<u32>(Gui::FlexDirection::RowReverse);
            }
            if (v == "column")
            {
                return static_cast<u32>(Gui::FlexDirection::Column);
            }
            if (v == "column-reverse")
            {
                return static_cast<u32>(Gui::FlexDirection::ColumnReverse);
            }
            return std::nullopt;
        }

        optional<u32> ParseJustify(std::string_view v)
        {
            if (v == "flex-start")
            {
                return static_cast<u32>(Gui::Justify::FlexStart);
            }
            if (v == "center")
            {
                return static_cast<u32>(Gui::Justify::Center);
            }
            if (v == "flex-end")
            {
                return static_cast<u32>(Gui::Justify::FlexEnd);
            }
            if (v == "space-between")
            {
                return static_cast<u32>(Gui::Justify::SpaceBetween);
            }
            if (v == "space-around")
            {
                return static_cast<u32>(Gui::Justify::SpaceAround);
            }
            if (v == "space-evenly")
            {
                return static_cast<u32>(Gui::Justify::SpaceEvenly);
            }
            return std::nullopt;
        }

        optional<u32> ParseAlign(std::string_view v)
        {
            if (v == "auto")
            {
                return static_cast<u32>(Gui::Align::Auto);
            }
            if (v == "flex-start")
            {
                return static_cast<u32>(Gui::Align::FlexStart);
            }
            if (v == "center")
            {
                return static_cast<u32>(Gui::Align::Center);
            }
            if (v == "flex-end")
            {
                return static_cast<u32>(Gui::Align::FlexEnd);
            }
            if (v == "stretch")
            {
                return static_cast<u32>(Gui::Align::Stretch);
            }
            return std::nullopt;
        }

        optional<u32> ParseWrap(std::string_view v)
        {
            if (v == "nowrap")
            {
                return static_cast<u32>(Gui::FlexWrap::NoWrap);
            }
            if (v == "wrap")
            {
                return static_cast<u32>(Gui::FlexWrap::Wrap);
            }
            if (v == "wrap-reverse")
            {
                return static_cast<u32>(Gui::FlexWrap::WrapReverse);
            }
            return std::nullopt;
        }

        optional<u32> ParsePosition(std::string_view v)
        {
            if (v == "relative")
            {
                return static_cast<u32>(Gui::PositionType::Relative);
            }
            if (v == "absolute")
            {
                return static_cast<u32>(Gui::PositionType::Absolute);
            }
            return std::nullopt;
        }

        optional<u32> ParsePointerEvents(std::string_view v)
        {
            if (v == "auto")
            {
                return static_cast<u32>(Gui::PointerEvents::Auto);
            }
            if (v == "none")
            {
                return static_cast<u32>(Gui::PointerEvents::None);
            }
            if (v == "children")
            {
                return static_cast<u32>(Gui::PointerEvents::Children);
            }
            return std::nullopt;
        }

        // Splits a value on runs of whitespace, for the keyword shorthands.
        vector<std::string_view> SplitWhitespace(std::string_view value)
        {
            vector<std::string_view> parts;
            usize i = 0;
            const usize n = value.size();
            while (i < n)
            {
                while (i < n && std::isspace(static_cast<unsigned char>(value[i])) != 0)
                {
                    ++i;
                }
                const usize start = i;
                while (i < n && std::isspace(static_cast<unsigned char>(value[i])) == 0)
                {
                    ++i;
                }
                if (i > start)
                {
                    parts.push_back(value.substr(start, i - start));
                }
            }
            return parts;
        }

        // The CSS `overflow` keyword set. `clip` is accepted as a synonym for `hidden` and `auto`
        // for `scroll`: veng shows a scrollbar only on a scrollable axis whose content overflows,
        // so the CSS auto/scroll distinction (always-visible bar) has no separate meaning here.
        optional<u32> ParseOverflow(std::string_view v)
        {
            if (v == "visible")
            {
                return static_cast<u32>(Gui::Overflow::Visible);
            }
            if (v == "hidden" || v == "clip")
            {
                return static_cast<u32>(Gui::Overflow::Hidden);
            }
            if (v == "scroll" || v == "auto")
            {
                return static_cast<u32>(Gui::Overflow::Scroll);
            }
            return std::nullopt;
        }

        optional<u32> ParseScrollbarLayout(std::string_view v)
        {
            if (v == "overlay")
            {
                return static_cast<u32>(Gui::ScrollbarLayout::Overlay);
            }
            if (v == "gutter")
            {
                return static_cast<u32>(Gui::ScrollbarLayout::Gutter);
            }
            return std::nullopt;
        }

        optional<u32> ParseTextAlign(std::string_view v)
        {
            if (v == "left")
            {
                return static_cast<u32>(Gui::TextAlign::Left);
            }
            if (v == "center")
            {
                return static_cast<u32>(Gui::TextAlign::Center);
            }
            if (v == "right")
            {
                return static_cast<u32>(Gui::TextAlign::Right);
            }
            return std::nullopt;
        }

        // Builds a CookedStyleProperty for an enum-valued property, or a located error.
        Result<CookedStyleProperty> EnumProperty(StyleProperty property, optional<u32> ordinal,
                                                 std::string_view value, const string& located)
        {
            if (!ordinal)
            {
                return std::unexpected(fmt::format("{}: '{}' is not a valid value for '{}'",
                                                   located, value, ToString(property)));
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Unit = *ordinal;
            return cp;
        }

        Result<CookedStyleProperty> ScalarProperty(StyleProperty property, std::string_view value,
                                                   const string& located)
        {
            const Result<f32> v = ParseFloat(value, located);
            if (!v)
            {
                return std::unexpected(v.error());
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Values[0] = *v;
            return cp;
        }

        Result<CookedStyleProperty> LengthProperty(StyleProperty property, std::string_view value,
                                                   const string& located)
        {
            const Result<std::pair<u32, f32>> len = ParseLength(value, located);
            if (!len)
            {
                return std::unexpected(len.error());
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Unit = len->first;
            cp.Values[0] = len->second;
            return cp;
        }

        Result<CookedStyleProperty> ColorProperty(StyleProperty property, std::string_view value,
                                                  const string& located)
        {
            const Result<vec4> color = ParseColorValue(value, located);
            if (!color)
            {
                return std::unexpected(color.error());
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Values[0] = color->r;
            cp.Values[1] = color->g;
            cp.Values[2] = color->b;
            cp.Values[3] = color->a;
            return cp;
        }

        Result<CookedStyleProperty> EdgeProperty(StyleProperty property, std::string_view value,
                                                 const string& located)
        {
            const Result<vec4> edges = ParseEdgeShorthand(value, located);
            if (!edges)
            {
                return std::unexpected(edges.error());
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Values[0] = edges->x;
            cp.Values[1] = edges->y;
            cp.Values[2] = edges->z;
            cp.Values[3] = edges->w;
            return cp;
        }
    }

    Result<vec4> ParseStyleColor(std::string_view value, const string& located)
    {
        return ParseColorValue(value, located);
    }

    Result<CookedStyleProperty> ParseStyleDeclaration(StyleProperty property,
                                                      std::string_view value, const string& located)
    {
        const std::string_view v = Trim(value);
        if (v.empty())
        {
            return std::unexpected(
                fmt::format("{}: '{}' has an empty value", located, ToString(property)));
        }

        switch (property)
        {
        case StyleProperty::FlexDirection:
            return EnumProperty(property, ParseFlexDirection(v), v, located);
        case StyleProperty::JustifyContent:
            return EnumProperty(property, ParseJustify(v), v, located);
        case StyleProperty::AlignItems:
        case StyleProperty::AlignSelf:
            return EnumProperty(property, ParseAlign(v), v, located);
        case StyleProperty::FlexWrap:
            return EnumProperty(property, ParseWrap(v), v, located);
        case StyleProperty::Position:
            return EnumProperty(property, ParsePosition(v), v, located);
        case StyleProperty::PointerEvents:
            return EnumProperty(property, ParsePointerEvents(v), v, located);
        case StyleProperty::TextAlign:
            return EnumProperty(property, ParseTextAlign(v), v, located);
        case StyleProperty::OverflowX:
        case StyleProperty::OverflowY:
            return EnumProperty(property, ParseOverflow(v), v, located);
        case StyleProperty::ScrollbarLayout:
            return EnumProperty(property, ParseScrollbarLayout(v), v, located);

        case StyleProperty::Overflow:
        {
            // The CSS two-value shorthand: `overflow: <x> <y>`, one value applying to both axes.
            // Both ordinals ride Values (not Unit, which holds a single enumerator), so the
            // runtime writes the pair in one declaration.
            const vector<std::string_view> parts = SplitWhitespace(v);
            if (parts.empty() || parts.size() > 2)
            {
                return std::unexpected(fmt::format("{}: 'overflow' expects 1 or 2 keywords, got {}",
                                                   located, parts.size()));
            }
            const optional<u32> x = ParseOverflow(parts[0]);
            const optional<u32> y = ParseOverflow(parts.size() == 2 ? parts[1] : parts[0]);
            if (!x || !y)
            {
                return std::unexpected(
                    fmt::format("{}: '{}' is not a valid value for 'overflow'", located, v));
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Values[0] = static_cast<f32>(*x);
            cp.Values[1] = static_cast<f32>(*y);
            return cp;
        }

        case StyleProperty::FlexGrow:
        case StyleProperty::FlexShrink:
        case StyleProperty::BorderWidth:
        case StyleProperty::TextSize:
        case StyleProperty::Opacity:
        case StyleProperty::Rotation:
        case StyleProperty::InsetLeft:
        case StyleProperty::InsetTop:
        case StyleProperty::InsetRight:
        case StyleProperty::InsetBottom:
            return ScalarProperty(property, v, located);

        case StyleProperty::FlexBasis:
        case StyleProperty::Width:
        case StyleProperty::Height:
        case StyleProperty::MinWidth:
        case StyleProperty::MinHeight:
        case StyleProperty::MaxWidth:
        case StyleProperty::MaxHeight:
            return LengthProperty(property, v, located);

        case StyleProperty::Margin:
        case StyleProperty::Padding:
        case StyleProperty::Inset:
        case StyleProperty::CornerRadius:
            return EdgeProperty(property, v, located);

        case StyleProperty::Background:
        case StyleProperty::BorderColor:
        case StyleProperty::TextColor:
            return ColorProperty(property, v, located);

        case StyleProperty::TextFont:
        {
            const optional<AssetId> id = ParseAssetId(v);
            if (!id)
            {
                return std::unexpected(
                    fmt::format("{}: 'font' must be a hex AssetId (0x…), got '{}'", located, v));
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Handle = id->Value;
            return cp;
        }

        case StyleProperty::Animation:
            // An animation reference resolves against a stylesheet's own @keyframes table, so
            // only the stylesheet importer's rule parse can cook it; anywhere else (an inline
            // style, a keyframe block) it has no clip table to resolve into.
            return std::unexpected(
                fmt::format("{}: 'animation' is only authorable in a stylesheet rule", located));

        case StyleProperty::BackgroundGradient:
            // A gradient bakes a ramp into the stylesheet's own gradient table, so only the
            // stylesheet importer's rule parse can cook it; an inline style or keyframe block has
            // no gradient table to append to.
            return std::unexpected(fmt::format(
                "{}: 'background-gradient' is only authorable in a stylesheet rule", located));

        case StyleProperty::Origin:
        {
            // One or two bare normalized fractions; one value applies to both axes.
            vector<f32> parts;
            usize i = 0;
            while (i < v.size())
            {
                while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i])) != 0)
                {
                    ++i;
                }
                const usize start = i;
                while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i])) == 0)
                {
                    ++i;
                }
                if (i > start)
                {
                    const Result<f32> part = ParseFloat(v.substr(start, i - start), located);
                    if (!part)
                    {
                        return std::unexpected(part.error());
                    }
                    parts.push_back(*part);
                }
            }
            if (parts.empty() || parts.size() > 2)
            {
                return std::unexpected(fmt::format(
                    "{}: 'origin' expects one or two normalized fractions, got '{}'", located, v));
            }
            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(property);
            cp.Values[0] = parts[0];
            cp.Values[1] = parts.size() == 2 ? parts[1] : parts[0];
            return cp;
        }
        }

        return std::unexpected(
            fmt::format("{}: unhandled style property '{}'", located, ToString(property)));
    }
}
