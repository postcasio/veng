#include "StyleSheetImporter.h"

#include "CssTokenizer.h"
#include "StyleParse.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/StyleProperty.h>

namespace Veng::Cook
{
    namespace
    {
        // One parsed selector: an element type, a class, an id (any of which may be empty for a
        // wildcard), and the pseudo-state it is scoped to (None for the base state).
        struct ParsedSelector
        {
            string Type;
            string Class;
            string Id;
            Gui::ElementState State = Gui::ElementState::None;
        };

        optional<Gui::ElementState> ParsePseudoState(std::string_view name)
        {
            if (name == "hover")
            {
                return Gui::ElementState::Hovered;
            }
            if (name == "active")
            {
                return Gui::ElementState::Active;
            }
            if (name == "focus")
            {
                return Gui::ElementState::Focused;
            }
            if (name == "disabled")
            {
                return Gui::ElementState::Disabled;
            }
            if (name == "checked")
            {
                return Gui::ElementState::Checked;
            }
            if (name == "selected")
            {
                return Gui::ElementState::Selected;
            }
            return std::nullopt;
        }

        // Copies a name into a fixed-capacity nul-terminated char array, truncating at capacity - 1.
        void CopyName(char* dst, usize capacity, const string& name)
        {
            const usize count = std::min(name.size(), capacity - 1);
            std::memcpy(dst, name.data(), count);
            dst[count] = '\0';
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }

        // Parses one comma-separated selector run (a sequence of tokens up to `{`) into one
        // ParsedSelector. A run is `Type`, `.class`, `#id`, `:state`, in any order and combination.
        Result<ParsedSelector> ParseSelector(const std::vector<CssToken>& tokens, usize& i,
                                             const string& located)
        {
            ParsedSelector selector;
            bool any = false;

            while (i < tokens.size())
            {
                const CssToken& token = tokens[i];
                if (token.Kind == CssTokenKind::Comma || token.Kind == CssTokenKind::LBrace)
                {
                    break;
                }

                if (token.Kind == CssTokenKind::Ident)
                {
                    if (!selector.Type.empty())
                    {
                        return std::unexpected(fmt::format(
                            "{}: selector names more than one element type ('{}' and '{}')",
                            located, selector.Type, token.Text));
                    }
                    selector.Type = token.Text;
                    any = true;
                    ++i;
                    continue;
                }
                if (token.Kind == CssTokenKind::Dot)
                {
                    ++i;
                    if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Ident)
                    {
                        return std::unexpected(
                            fmt::format("{}: '.' must be followed by a class name", located));
                    }
                    selector.Class = tokens[i].Text;
                    any = true;
                    ++i;
                    continue;
                }
                if (token.Kind == CssTokenKind::Hash)
                {
                    ++i;
                    if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Ident)
                    {
                        return std::unexpected(
                            fmt::format("{}: '#' must be followed by an id", located));
                    }
                    selector.Id = tokens[i].Text;
                    any = true;
                    ++i;
                    continue;
                }
                if (token.Kind == CssTokenKind::Colon)
                {
                    ++i;
                    if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Ident)
                    {
                        return std::unexpected(fmt::format(
                            "{}: ':' must be followed by a pseudo-state name", located));
                    }
                    const optional<Gui::ElementState> state = ParsePseudoState(tokens[i].Text);
                    if (!state)
                    {
                        return std::unexpected(fmt::format(
                            "{}: unknown pseudo-state ':{}' (expected hover/active/focus/disabled/"
                            "checked)",
                            located, tokens[i].Text));
                    }
                    selector.State = *state;
                    any = true;
                    ++i;
                    continue;
                }

                return std::unexpected(fmt::format("{}: unexpected token in selector", located));
            }

            if (!any)
            {
                return std::unexpected(fmt::format("{}: empty selector", located));
            }
            return selector;
        }

        // Parses an `animation: <name> <duration>[s] [loop|ping-pong|once]` declaration value,
        // resolving the clip name against the sheet's @keyframes table.
        Result<CookedStyleProperty> ParseAnimationDeclaration(const string& value,
                                                              const vector<string>& animationNames,
                                                              const string& located)
        {
            vector<string> parts;
            usize start = 0;
            while (start < value.size())
            {
                const usize space = value.find(' ', start);
                const usize end = space == string::npos ? value.size() : space;
                if (end > start)
                {
                    parts.emplace_back(value.substr(start, end - start));
                }
                start = end + 1;
            }
            if (parts.size() < 2 || parts.size() > 3)
            {
                return std::unexpected(fmt::format(
                    "{}: 'animation' expects '<name> <duration> [loop|ping-pong|once]', got '{}'",
                    located, value));
            }

            const auto clip = std::ranges::find(animationNames, parts[0]);
            if (clip == animationNames.end())
            {
                return std::unexpected(fmt::format(
                    "{}: 'animation' names unknown @keyframes clip '{}'", located, parts[0]));
            }

            std::string_view durationText = parts[1];
            if (!durationText.empty() && durationText.back() == 's')
            {
                durationText.remove_suffix(1);
            }
            f32 duration = 0.0f;
            const auto [ptr, ec] = std::from_chars(
                durationText.data(), durationText.data() + durationText.size(), duration);
            if (ec != std::errc{} || ptr != durationText.data() + durationText.size() ||
                duration <= 0.0f)
            {
                return std::unexpected(fmt::format(
                    "{}: 'animation' duration must be a positive seconds value, got '{}'", located,
                    parts[1]));
            }

            u32 mode = static_cast<u32>(Gui::AnimationLoopMode::Loop);
            if (parts.size() == 3)
            {
                if (parts[2] == "loop")
                {
                    mode = static_cast<u32>(Gui::AnimationLoopMode::Loop);
                }
                else if (parts[2] == "ping-pong")
                {
                    mode = static_cast<u32>(Gui::AnimationLoopMode::PingPong);
                }
                else if (parts[2] == "once")
                {
                    mode = static_cast<u32>(Gui::AnimationLoopMode::Once);
                }
                else
                {
                    return std::unexpected(fmt::format(
                        "{}: 'animation' mode must be loop, ping-pong, or once; got '{}'", located,
                        parts[2]));
                }
            }

            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(Gui::StyleProperty::Animation);
            cp.Unit = static_cast<u32>(clip - animationNames.begin());
            cp.Values[0] = duration;
            cp.Values[1] = static_cast<f32>(mode);
            return cp;
        }

        // Splits a string on `delim` at parenthesis depth zero, trimming surrounding whitespace from
        // each part. A `delim` nested inside parentheses (an rgb()/rgba() color's component commas)
        // stays within its part, so a functional color survives as one stop.
        vector<string> SplitTrim(std::string_view text, char delim)
        {
            vector<string> out;
            usize depth = 0;
            usize start = 0;
            const auto emit = [&](usize end)
            {
                const std::string_view part = text.substr(start, end - start);
                usize b = 0;
                usize e = part.size();
                while (b < e && std::isspace(static_cast<unsigned char>(part[b])) != 0)
                {
                    ++b;
                }
                while (e > b && std::isspace(static_cast<unsigned char>(part[e - 1])) != 0)
                {
                    --e;
                }
                out.emplace_back(part.substr(b, e - b));
            };
            for (usize i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (c == '(')
                {
                    ++depth;
                }
                else if (c == ')' && depth > 0)
                {
                    --depth;
                }
                else if (c == delim && depth == 0)
                {
                    emit(i);
                    start = i + 1;
                }
            }
            emit(text.size());
            return out;
        }

        // Splits a string on runs of whitespace into non-empty tokens, treating a parenthesized
        // span (an rgb()/rgba() color, whose commas and spaces are internal) as one token.
        vector<string> WhitespaceTokens(std::string_view text)
        {
            vector<string> out;
            usize i = 0;
            usize depth = 0;
            while (i < text.size())
            {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0)
                {
                    ++i;
                }
                const usize start = i;
                while (i < text.size() &&
                       (depth > 0 || std::isspace(static_cast<unsigned char>(text[i])) == 0))
                {
                    const char c = text[i];
                    if (c == '(')
                    {
                        ++depth;
                    }
                    else if (c == ')' && depth > 0)
                    {
                        --depth;
                    }
                    ++i;
                }
                if (i > start)
                {
                    out.emplace_back(text.substr(start, i - start));
                }
            }
            return out;
        }

        // Parses a bare number with an optional `deg` suffix into degrees.
        Result<f32> ParseAngle(std::string_view token, const string& located)
        {
            std::string_view t = token;
            if (t.size() >= 3 && t.substr(t.size() - 3) == "deg")
            {
                t = t.substr(0, t.size() - 3);
            }
            f32 value = 0.0f;
            const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), value);
            if (ec != std::errc{} || ptr != t.data() + t.size())
            {
                return std::unexpected(
                    fmt::format("{}: expected an angle like '135deg', got '{}'", located, token));
            }
            return value;
        }

        // Parses an `N%` token into a [0, 1] fraction.
        Result<f32> ParsePercent(std::string_view token, const string& located)
        {
            std::string_view t = token;
            if (!t.empty() && t.back() == '%')
            {
                t = t.substr(0, t.size() - 1);
            }
            f32 value = 0.0f;
            const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), value);
            if (ec != std::errc{} || ptr != t.data() + t.size())
            {
                return std::unexpected(
                    fmt::format("{}: expected a percentage, got '{}'", located, token));
            }
            return value / 100.0f;
        }

        // One authored gradient stop: a linear straight-alpha color at a normalized position, with a
        // negative position meaning "unspecified" (assigned by even distribution before baking).
        struct GradientStop
        {
            vec4 Color{0.0f};
            f32 Pos = -1.0f;
        };

        // Bakes a stop list (positions assigned + clamped non-decreasing) into an N×1 RGBA16Sfloat
        // ramp (four half-floats per texel), interpolating between bracketing stops in linear space
        // (the colors are already linear). No brightness clamp — an HDR (> 1) stop is preserved so a
        // gradient can glow across its extent.
        vector<u8> BakeGradientRamp(const vector<GradientStop>& stops)
        {
            constexpr u32 RampTexels = 256;
            constexpr usize BytesPerTexel = 4 * sizeof(u16);
            vector<u8> ramp(static_cast<usize>(RampTexels) * BytesPerTexel);
            for (u32 i = 0; i < RampTexels; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(RampTexels - 1);
                vec4 color = stops.back().Color;
                if (t <= stops.front().Pos)
                {
                    color = stops.front().Color;
                }
                else if (t < stops.back().Pos)
                {
                    for (usize s = 1; s < stops.size(); ++s)
                    {
                        if (t <= stops[s].Pos)
                        {
                            const f32 span = stops[s].Pos - stops[s - 1].Pos;
                            const f32 u = span > 0.0f ? (t - stops[s - 1].Pos) / span : 0.0f;
                            color = glm::mix(stops[s - 1].Color, stops[s].Color, u);
                            break;
                        }
                    }
                }
                const u16 halves[4] = {
                    static_cast<u16>(glm::packHalf1x16(color.r)),
                    static_cast<u16>(glm::packHalf1x16(color.g)),
                    static_cast<u16>(glm::packHalf1x16(color.b)),
                    static_cast<u16>(glm::packHalf1x16(color.a)),
                };
                std::memcpy(ramp.data() + static_cast<usize>(i) * BytesPerTexel, halves,
                            sizeof(halves));
            }
            return ramp;
        }

        // Parses a `background-gradient: <kind> [geometry], <color> [pos%], …` value: it resolves the
        // shape + box-space geometry, bakes the multi-stop color into a ramp appended to `rampBytes`,
        // records a CookedStyleGradient in `gradients`, and returns a declaration referencing it by
        // index. Geometry is packed in the element's normalized box space ([-1, 1]); positions along
        // a gradient are baked into the ramp, so the runtime evaluates one t per fragment and samples.
        Result<CookedStyleProperty> ParseGradientDeclaration(const string& value,
                                                             const string& located,
                                                             vector<CookedStyleGradient>& gradients,
                                                             vector<u8>& rampBytes)
        {
            constexpr f32 Pi = 3.14159265358979323846f;
            const vector<string> parts = SplitTrim(value, ',');
            if (parts.size() < 3)
            {
                return std::unexpected(fmt::format(
                    "{}: 'background-gradient' expects '<kind> [geometry], <color> [pos%], "
                    "<color> [pos%] …' (at least two stops), got '{}'",
                    located, value));
            }

            const vector<string> header = WhitespaceTokens(parts[0]);
            if (header.empty())
            {
                return std::unexpected(
                    fmt::format("{}: 'background-gradient' is missing its kind", located));
            }

            // A percentage token maps to a normalized box coordinate: 0% → -1, 100% → +1.
            const auto boxCoord = [&](const string& token) -> Result<f32>
            {
                const Result<f32> fraction = ParsePercent(token, located);
                if (!fraction)
                {
                    return std::unexpected(fraction.error());
                }
                return *fraction * 2.0f - 1.0f;
            };

            Gui::GradientKind kind = Gui::GradientKind::Linear;
            vec2 p0(0.0f);
            vec2 p1(0.0f);
            f32 angleOffset = 0.0f;
            const string& kindName = header[0];
            if (kindName == "linear")
            {
                kind = Gui::GradientKind::Linear;
                if (header.size() >= 2 && header[1] == "from")
                {
                    // Explicit endpoints: linear from <x%> <y%> to <x%> <y%>, in box space.
                    if (header.size() != 7 || header[4] != "to")
                    {
                        return std::unexpected(fmt::format(
                            "{}: linear 'from' expects 'from <x%> <y%> to <x%> <y%>', got '{}'",
                            located, parts[0]));
                    }
                    const Result<f32> x0 = boxCoord(header[2]);
                    const Result<f32> y0 = boxCoord(header[3]);
                    const Result<f32> x1 = boxCoord(header[5]);
                    const Result<f32> y1 = boxCoord(header[6]);
                    for (const Result<f32>* v : {&x0, &y0, &x1, &y1})
                    {
                        if (!*v)
                        {
                            return std::unexpected(v->error());
                        }
                    }
                    p0 = vec2(*x0, *y0);
                    p1 = vec2(*x1, *y1);
                }
                else
                {
                    f32 degrees = 180.0f;
                    if (header.size() >= 2)
                    {
                        const Result<f32> angle = ParseAngle(header[1], located);
                        if (!angle)
                        {
                            return std::unexpected(angle.error());
                        }
                        degrees = *angle;
                    }
                    if (header.size() > 2)
                    {
                        return std::unexpected(fmt::format(
                            "{}: a linear gradient takes one angle or explicit endpoints, got '{}'",
                            located, parts[0]));
                    }
                    // CSS angle: 0deg points to the top, growing clockwise; box y is down. The
                    // box-fit endpoints span the box's projection onto the direction, so t runs 0→1
                    // across the whole box along the axis.
                    const f32 radians = degrees * (Pi / 180.0f);
                    const vec2 direction(std::sin(radians), -std::cos(radians));
                    const f32 span = std::abs(direction.x) + std::abs(direction.y);
                    const f32 halfSpan = span > 1e-6f ? span : 1.0f;
                    p0 = -halfSpan * direction;
                    p1 = halfSpan * direction;
                }
            }
            else if (kindName == "radial" || kindName == "conic")
            {
                f32 centerX = 0.5f;
                f32 centerY = 0.5f;
                f32 fromDegrees = 0.0f;
                optional<vec2> radii;
                usize k = 1;
                if (kindName == "conic" && k < header.size() && header[k] == "from")
                {
                    if (k + 1 >= header.size())
                    {
                        return std::unexpected(fmt::format(
                            "{}: conic 'from' needs an angle (e.g. 'from 90deg')", located));
                    }
                    const Result<f32> angle = ParseAngle(header[k + 1], located);
                    if (!angle)
                    {
                        return std::unexpected(angle.error());
                    }
                    fromDegrees = *angle;
                    k += 2;
                }
                if (k < header.size() && header[k] == "at")
                {
                    if (k + 2 >= header.size())
                    {
                        return std::unexpected(fmt::format(
                            "{}: gradient 'at' needs two percentages (e.g. 'at 50% 50%')",
                            located));
                    }
                    const Result<f32> x = ParsePercent(header[k + 1], located);
                    const Result<f32> y = ParsePercent(header[k + 2], located);
                    if (!x)
                    {
                        return std::unexpected(x.error());
                    }
                    if (!y)
                    {
                        return std::unexpected(y.error());
                    }
                    centerX = *x;
                    centerY = *y;
                    k += 3;
                }
                if (kindName == "radial" && k < header.size() && header[k] == "radius")
                {
                    // radius <r%> (circular) or radius <rx%> <ry%> (ellipse), as a fraction of the
                    // box half-extent (100% reaches the box edge along that axis).
                    const usize count = header.size() - (k + 1);
                    if (count != 1 && count != 2)
                    {
                        return std::unexpected(fmt::format(
                            "{}: gradient 'radius' needs one or two percentages", located));
                    }
                    const Result<f32> rx = ParsePercent(header[k + 1], located);
                    if (!rx)
                    {
                        return std::unexpected(rx.error());
                    }
                    f32 ry = *rx;
                    if (count == 2)
                    {
                        const Result<f32> parsed = ParsePercent(header[k + 2], located);
                        if (!parsed)
                        {
                            return std::unexpected(parsed.error());
                        }
                        ry = *parsed;
                    }
                    radii = vec2(*rx, ry);
                    k += count + 1;
                }
                if (k < header.size())
                {
                    return std::unexpected(
                        fmt::format("{}: unexpected '{}' in {} gradient geometry", located,
                                    header[k], kindName));
                }

                const vec2 center(centerX * 2.0f - 1.0f, centerY * 2.0f - 1.0f);
                p0 = center;
                if (kindName == "radial")
                {
                    kind = Gui::GradientKind::Radial;
                    if (radii)
                    {
                        p1 = *radii;
                    }
                    else
                    {
                        // Farthest-corner circular fit (the CSS default): t reaches 1 at the box
                        // corner most distant from the center, so the whole box is covered.
                        f32 radius = 0.0f;
                        for (const f32 cornerX : {-1.0f, 1.0f})
                        {
                            for (const f32 cornerY : {-1.0f, 1.0f})
                            {
                                radius =
                                    std::max(radius, glm::length(vec2(cornerX, cornerY) - center));
                            }
                        }
                        p1 = vec2(radius, radius);
                    }
                }
                else
                {
                    kind = Gui::GradientKind::Conic;
                    angleOffset = fromDegrees / 360.0f;
                }
            }
            else
            {
                return std::unexpected(
                    fmt::format("{}: unknown gradient kind '{}' (expected linear/radial/conic)",
                                located, kindName));
            }

            vector<GradientStop> stops;
            for (usize s = 1; s < parts.size(); ++s)
            {
                const vector<string> tokens = WhitespaceTokens(parts[s]);
                if (tokens.empty())
                {
                    return std::unexpected(
                        fmt::format("{}: empty gradient stop in '{}'", located, value));
                }
                const Result<vec4> color = ParseStyleColor(tokens[0], located);
                if (!color)
                {
                    return std::unexpected(color.error());
                }
                GradientStop stop{.Color = *color, .Pos = -1.0f};
                if (tokens.size() >= 2)
                {
                    const Result<f32> pos = ParsePercent(tokens[1], located);
                    if (!pos)
                    {
                        return std::unexpected(pos.error());
                    }
                    stop.Pos = *pos;
                }
                if (tokens.size() > 2)
                {
                    return std::unexpected(fmt::format(
                        "{}: a gradient stop is '<color> [<pos>%]', got '{}'", located, parts[s]));
                }
                stops.push_back(stop);
            }

            // Assign each unspecified position by even distribution, then clamp to [0, 1] and force a
            // non-decreasing sequence (a stop never precedes the one before it — the CSS fixup).
            const usize count = stops.size();
            for (usize s = 0; s < count; ++s)
            {
                if (stops[s].Pos < 0.0f)
                {
                    stops[s].Pos =
                        count == 1 ? 0.0f : static_cast<f32>(s) / static_cast<f32>(count - 1);
                }
            }
            f32 previous = 0.0f;
            for (GradientStop& stop : stops)
            {
                stop.Pos = std::max(std::clamp(stop.Pos, 0.0f, 1.0f), previous);
                previous = stop.Pos;
            }

            const vector<u8> ramp = BakeGradientRamp(stops);
            CookedStyleGradient cooked{};
            cooked.Kind = static_cast<u32>(kind);
            cooked.P0[0] = p0.x;
            cooked.P0[1] = p0.y;
            cooked.P1[0] = p1.x;
            cooked.P1[1] = p1.y;
            cooked.AngleOffset = angleOffset;
            cooked.RampOffset = static_cast<u32>(rampBytes.size());
            cooked.RampTexels = static_cast<u32>(ramp.size() / (4 * sizeof(u16)));

            CookedStyleProperty cp{};
            cp.Property = static_cast<u32>(Gui::StyleProperty::BackgroundGradient);
            cp.Unit = static_cast<u32>(gradients.size());
            gradients.push_back(cooked);
            rampBytes.insert(rampBytes.end(), ramp.begin(), ramp.end());
            return cp;
        }

        // Parses a declaration block (the tokens between `{` and `}`) into cooked properties.
        // `animationNames` is the sheet's @keyframes table an `animation` declaration resolves
        // against; `gradients`/`rampBytes` are the sheet's gradient tables a `background-gradient`
        // appends to. All three are nullptr where those references are not authorable (a keyframe
        // block), so they fall through to ParseStyleDeclaration's located error.
        Result<vector<CookedStyleProperty>> ParseBlock(const std::vector<CssToken>& tokens,
                                                       usize& i, const string& located,
                                                       const vector<string>* animationNames,
                                                       vector<CookedStyleGradient>* gradients,
                                                       vector<u8>* rampBytes)
        {
            vector<CookedStyleProperty> properties;

            while (i < tokens.size() && tokens[i].Kind != CssTokenKind::RBrace)
            {
                // property name
                if (tokens[i].Kind != CssTokenKind::Ident)
                {
                    return std::unexpected(
                        fmt::format("{}: expected a property name in a declaration", located));
                }
                const string propertyName = tokens[i].Text;
                ++i;

                if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Colon)
                {
                    return std::unexpected(fmt::format("{}: property '{}' must be followed by ':'",
                                                       located, propertyName));
                }
                ++i;

                // Collect value tokens up to the `;` or `}`, rejoined with single spaces (a color's
                // `#` + hex recombines, a shorthand's parts stay space-separated).
                string value;
                while (i < tokens.size() && tokens[i].Kind != CssTokenKind::Semicolon &&
                       tokens[i].Kind != CssTokenKind::RBrace)
                {
                    switch (tokens[i].Kind)
                    {
                    case CssTokenKind::Hash:
                        value += '#';
                        break;
                    case CssTokenKind::Ident:
                    case CssTokenKind::Value:
                        if (!value.empty() && value.back() != '#' && value.back() != '(')
                        {
                            value += ' ';
                        }
                        value += tokens[i].Text;
                        break;
                    case CssTokenKind::Dot:
                        value += '.';
                        break;
                    case CssTokenKind::Comma:
                        // Commas separate list values (a gradient's kind/geometry and its stops, an
                        // rgb()/rgba() color's components); the consumer splits on them.
                        value += ',';
                        break;
                    case CssTokenKind::LParen:
                        // Parentheses wrap a functional color's component list (rgb()/rgba()); the
                        // color parser reads them and splits the inner commas.
                        value += '(';
                        break;
                    case CssTokenKind::RParen:
                        value += ')';
                        break;
                    default:
                        return std::unexpected(fmt::format(
                            "{}: unexpected token in the value of '{}'", located, propertyName));
                    }
                    ++i;
                }

                if (i < tokens.size() && tokens[i].Kind == CssTokenKind::Semicolon)
                {
                    ++i;
                }

                const optional<Gui::StyleProperty> property = Gui::ParseStyleProperty(propertyName);
                if (!property)
                {
                    return std::unexpected(
                        fmt::format("{}: unknown style property '{}'", located, propertyName));
                }

                // An animation reference resolves against the sheet's clip table here; with no
                // table (a keyframe block) it falls through to ParseStyleDeclaration's error.
                if (*property == Gui::StyleProperty::Animation && animationNames != nullptr)
                {
                    const Result<CookedStyleProperty> cooked =
                        ParseAnimationDeclaration(value, *animationNames, located);
                    if (!cooked)
                    {
                        return std::unexpected(cooked.error());
                    }
                    properties.push_back(*cooked);
                    continue;
                }

                // A gradient bakes into the sheet's gradient table here; with no table (a keyframe
                // block) it falls through to ParseStyleDeclaration's located error.
                if (*property == Gui::StyleProperty::BackgroundGradient && gradients != nullptr &&
                    rampBytes != nullptr)
                {
                    const Result<CookedStyleProperty> cooked =
                        ParseGradientDeclaration(value, located, *gradients, *rampBytes);
                    if (!cooked)
                    {
                        return std::unexpected(cooked.error());
                    }
                    properties.push_back(*cooked);
                    continue;
                }

                // The box-shadow shorthand cooks to two declarations (geometry and color), so it
                // cannot ride the one-declaration return the other properties share.
                if (*property == Gui::StyleProperty::BoxShadow)
                {
                    const Result<vector<CookedStyleProperty>> shadow =
                        ParseBoxShadowDeclaration(value, located);
                    if (!shadow)
                    {
                        return std::unexpected(shadow.error());
                    }
                    properties.insert(properties.end(), shadow->begin(), shadow->end());
                    continue;
                }

                const Result<CookedStyleProperty> cooked =
                    ParseStyleDeclaration(*property, value, located);
                if (!cooked)
                {
                    return std::unexpected(cooked.error());
                }
                properties.push_back(*cooked);
            }

            if (i >= tokens.size())
            {
                return std::unexpected(fmt::format("{}: unterminated declaration block", located));
            }
            ++i; // consume the '}'
            if (const VoidResult exclusive = CheckExclusiveFillSources(properties, located);
                !exclusive)
            {
                return std::unexpected(exclusive.error());
            }
            return properties;
        }

        // Whether the cursor sits on a `@keyframes` at-rule (lexed as one value run).
        bool IsKeyframesAt(const std::vector<CssToken>& tokens, usize i)
        {
            return i < tokens.size() && tokens[i].Kind == CssTokenKind::Value &&
                   tokens[i].Text == "@keyframes";
        }

        // Advances past one top-level statement — its prelude and its brace block, nested blocks
        // included. False when no block opens (or the block is unterminated) before EOF.
        bool SkipStatement(const std::vector<CssToken>& tokens, usize& i)
        {
            while (i < tokens.size() && tokens[i].Kind != CssTokenKind::LBrace)
            {
                ++i;
            }
            if (i >= tokens.size())
            {
                return false;
            }
            usize depth = 0;
            do
            {
                if (tokens[i].Kind == CssTokenKind::LBrace)
                {
                    ++depth;
                }
                else if (tokens[i].Kind == CssTokenKind::RBrace)
                {
                    --depth;
                }
                ++i;
            } while (i < tokens.size() && depth > 0);
            return depth == 0;
        }

        // Parses one `@keyframes <name> { <offsets> { declarations } ... }` clip, appending its
        // keyframes (sorted ascending by offset) and their declarations to the shared tables.
        VoidResult ParseKeyframes(const std::vector<CssToken>& tokens, usize& i,
                                  const string& located, vector<CookedStyleProperty>& properties,
                                  vector<CookedStyleAnimation>& animations,
                                  vector<CookedStyleKeyframe>& keyframes,
                                  vector<string>& animationNames)
        {
            ++i; // consume "@keyframes"
            if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Ident)
            {
                return std::unexpected(
                    fmt::format("{}: '@keyframes' must be followed by a clip name", located));
            }
            const string name = tokens[i].Text;
            if (std::ranges::find(animationNames, name) != animationNames.end())
            {
                return std::unexpected(
                    fmt::format("{}: duplicate @keyframes clip '{}'", located, name));
            }
            ++i;

            if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::LBrace)
            {
                return std::unexpected(
                    fmt::format("{}: expected '{{' after '@keyframes {}'", located, name));
            }
            ++i;

            vector<CookedStyleKeyframe> clipKeys;
            while (i < tokens.size() && tokens[i].Kind != CssTokenKind::RBrace)
            {
                // One or more comma-separated offsets (`from` / `to` / `NN%`) share a block.
                vector<f32> offsets;
                while (true)
                {
                    if (i >= tokens.size())
                    {
                        return std::unexpected(
                            fmt::format("{}: unterminated @keyframes '{}'", located, name));
                    }
                    const CssToken& token = tokens[i];
                    optional<f32> offset;
                    if (token.Kind == CssTokenKind::Ident && token.Text == "from")
                    {
                        offset = 0.0f;
                    }
                    else if (token.Kind == CssTokenKind::Ident && token.Text == "to")
                    {
                        offset = 1.0f;
                    }
                    else if (token.Kind == CssTokenKind::Value && !token.Text.empty() &&
                             token.Text.back() == '%')
                    {
                        f32 percent = 0.0f;
                        const char* first = token.Text.data();
                        const char* last = token.Text.data() + token.Text.size() - 1;
                        const auto [ptr, ec] = std::from_chars(first, last, percent);
                        if (ec == std::errc{} && ptr == last && percent >= 0.0f &&
                            percent <= 100.0f)
                        {
                            offset = percent / 100.0f;
                        }
                    }
                    if (!offset.has_value())
                    {
                        return std::unexpected(fmt::format(
                            "{}: @keyframes '{}': expected a keyframe offset (from/to/0..100%)",
                            located, name));
                    }
                    offsets.push_back(*offset);
                    ++i;
                    if (i < tokens.size() && tokens[i].Kind == CssTokenKind::Comma)
                    {
                        ++i;
                        continue;
                    }
                    break;
                }

                if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::LBrace)
                {
                    return std::unexpected(
                        fmt::format("{}: @keyframes '{}': expected '{{' after a keyframe offset",
                                    located, name));
                }
                ++i;

                const Result<vector<CookedStyleProperty>> block =
                    ParseBlock(tokens, i, located, nullptr, nullptr, nullptr);
                if (!block)
                {
                    return std::unexpected(block.error());
                }

                // Comma-listed offsets share one declaration slice in the property table.
                const u32 first = static_cast<u32>(properties.size());
                properties.insert(properties.end(), block->begin(), block->end());
                for (const f32 offset : offsets)
                {
                    clipKeys.push_back(
                        CookedStyleKeyframe{.Offset = offset,
                                            .FirstProperty = first,
                                            .PropertyCount = static_cast<u32>(block->size())});
                }
            }

            if (i >= tokens.size())
            {
                return std::unexpected(
                    fmt::format("{}: unterminated @keyframes '{}'", located, name));
            }
            ++i; // consume the '}'

            if (clipKeys.empty())
            {
                return std::unexpected(
                    fmt::format("{}: @keyframes '{}' declares no keyframes", located, name));
            }

            // The runtime interpolates over ascending offsets.
            std::ranges::sort(clipKeys, {}, &CookedStyleKeyframe::Offset);

            animations.push_back(
                CookedStyleAnimation{.FirstKeyframe = static_cast<u32>(keyframes.size()),
                                     .KeyframeCount = static_cast<u32>(clipKeys.size())});
            keyframes.insert(keyframes.end(), clipKeys.begin(), clipKeys.end());
            animationNames.push_back(name);
            return {};
        }

        // The file-scope variable table: a variable name (with its leading `--`) mapped to its
        // fully resolved token sequence (any nested var() already expanded at definition time, so a
        // use-site splice is a straight copy and cycles cannot form).
        using StyleVariableTable = unordered_map<string, vector<CssToken>>;

        // Rejoins a variable's token sequence into a declaration value string, matching the value
        // assembly a rule declaration uses (a `#` + hex recombines, a functional color's
        // parentheses are kept, other tokens stay space-separated), so the resolved value reads
        // identically to an inline literal.
        string AssembleValue(const vector<CssToken>& tokens)
        {
            string value;
            for (const CssToken& token : tokens)
            {
                switch (token.Kind)
                {
                case CssTokenKind::Hash:
                    value += '#';
                    break;
                case CssTokenKind::Ident:
                case CssTokenKind::Value:
                    if (!value.empty() && value.back() != '#' && value.back() != '(')
                    {
                        value += ' ';
                    }
                    value += token.Text;
                    break;
                case CssTokenKind::Dot:
                    value += '.';
                    break;
                case CssTokenKind::Comma:
                    value += ',';
                    break;
                case CssTokenKind::LParen:
                    value += '(';
                    break;
                case CssTokenKind::RParen:
                    value += ')';
                    break;
                default:
                    break;
                }
            }
            return value;
        }

        // Expands every `var(--name)` in a token run against `table`, returning the flattened run.
        // An undefined variable (define-before-use) or a malformed `var(...)` is a located error.
        Result<vector<CssToken>> SubstituteVars(const vector<CssToken>& tokens,
                                                const StyleVariableTable& table,
                                                const string& located)
        {
            vector<CssToken> out;
            usize i = 0;
            while (i < tokens.size())
            {
                if (tokens[i].Kind == CssTokenKind::Ident && tokens[i].Text == "var" &&
                    i + 1 < tokens.size() && tokens[i + 1].Kind == CssTokenKind::LParen)
                {
                    if (i + 3 >= tokens.size() || tokens[i + 2].Kind != CssTokenKind::Ident ||
                        tokens[i + 3].Kind != CssTokenKind::RParen)
                    {
                        return std::unexpected(
                            fmt::format("{}: malformed var(...); expected 'var(--name)'", located));
                    }
                    const string& name = tokens[i + 2].Text;
                    const auto found = table.find(name);
                    if (found == table.end())
                    {
                        return std::unexpected(
                            fmt::format("{}: use of undefined variable '{}'", located, name));
                    }
                    out.insert(out.end(), found->second.begin(), found->second.end());
                    i += 4;
                    continue;
                }
                out.push_back(tokens[i]);
                ++i;
            }
            return out;
        }

        VoidResult PreprocessSheet(const vector<CssToken>& tokens, const path& file,
                                   const CookContext& context, StyleVariableTable& table,
                                   vector<string>* ownNames, vector<CssToken>* substituted,
                                   std::set<string>& visited);

        // Reads a `@use`d sheet and merges its top-level variables (recursively honoring its own
        // `@use`s) into `table`, last-wins in encounter order. Rules are ignored — `@use` shares
        // variables only. The file is recorded as a build dependency so a theme edit re-cooks every
        // dependent sheet, and `visited` breaks an `@use` cycle (each file contributes once).
        VoidResult ProcessUse(const path& usedPath, const CookContext& context,
                              StyleVariableTable& table, std::set<string>& visited)
        {
            std::error_code ec;
            const path canonical = std::filesystem::weakly_canonical(usedPath, ec);
            const string key = (ec ? usedPath : canonical).string();
            if (visited.contains(key))
            {
                return {};
            }
            visited.insert(key);

            const std::ifstream in(usedPath, std::ios::binary);
            if (!in)
            {
                return std::unexpected(fmt::format(
                    "stylesheet importer: '{}': @use target cannot be opened", usedPath.string()));
            }
            std::stringstream buffer;
            buffer << in.rdbuf();
            const string source = buffer.str();

            context.RecordDependency(usedPath);

            const vector<CssToken> used = TokenizeCss(source);
            return PreprocessSheet(used, usedPath, context, table, nullptr, nullptr, visited);
        }

        // Linear top-down pass over a sheet's tokens: it resolves `@use` imports, collects file-scope
        // `--name: <tokens>;` variable declarations (define-before-use, last-wins), and — for the
        // sheet being cooked (`substituted` non-null) — emits every rule/`@keyframes` token with its
        // `var(--name)` uses expanded against the table state at each use site. `ownNames` (non-null
        // only for the sheet being cooked) records this file's own variable names for the runtime
        // table. A `@use` after a rule, a `--` inside a rule, an undefined variable, and a missing
        // `@use` target are located cook errors.
        VoidResult PreprocessSheet(const vector<CssToken>& tokens, const path& file,
                                   const CookContext& context, StyleVariableTable& table,
                                   vector<string>* ownNames, vector<CssToken>* substituted,
                                   std::set<string>& visited)
        {
            const string located = fmt::format("stylesheet importer: '{}'", file.string());

            // Whether the token at `idx` opens a `--name:` declaration (a variable at file scope, or
            // the file-scope-only error inside a rule).
            const auto isVariableDecl = [&](usize idx)
            {
                return tokens[idx].Kind == CssTokenKind::Ident && tokens[idx].Text.size() >= 2 &&
                       tokens[idx].Text[0] == '-' && tokens[idx].Text[1] == '-' &&
                       idx + 1 < tokens.size() && tokens[idx + 1].Kind == CssTokenKind::Colon;
            };

            usize depth = 0;
            bool seenRule = false;
            usize i = 0;
            while (i < tokens.size())
            {
                const CssToken& token = tokens[i];

                if (depth == 0)
                {
                    if (token.Kind == CssTokenKind::Value && token.Text == "@use")
                    {
                        if (seenRule)
                        {
                            return std::unexpected(
                                fmt::format("{}: '@use' must precede every rule", located));
                        }
                        ++i;
                        if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::String)
                        {
                            return std::unexpected(fmt::format(
                                "{}: '@use' expects a quoted path, e.g. '@use \"theme.vuss\";'",
                                located));
                        }
                        const path usedPath = file.parent_path() / path(tokens[i].Text);
                        ++i;
                        if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::Semicolon)
                        {
                            return std::unexpected(
                                fmt::format("{}: '@use' must end with ';'", located));
                        }
                        ++i;
                        const VoidResult used = ProcessUse(usedPath, context, table, visited);
                        if (!used)
                        {
                            return used;
                        }
                        continue;
                    }

                    if (isVariableDecl(i))
                    {
                        const string name = token.Text;
                        i += 2; // past the name and the ':'
                        vector<CssToken> valueTokens;
                        while (i < tokens.size() && tokens[i].Kind != CssTokenKind::Semicolon &&
                               tokens[i].Kind != CssTokenKind::RBrace)
                        {
                            valueTokens.push_back(tokens[i]);
                            ++i;
                        }
                        if (i < tokens.size() && tokens[i].Kind == CssTokenKind::Semicolon)
                        {
                            ++i;
                        }
                        // Resolve the value against the table as it stands now, so a variable
                        // referencing another sees the definition that precedes it.
                        const Result<vector<CssToken>> resolved =
                            SubstituteVars(valueTokens, table, located);
                        if (!resolved)
                        {
                            return std::unexpected(resolved.error());
                        }
                        table[name] = *resolved;
                        if (ownNames != nullptr &&
                            std::ranges::find(*ownNames, name) == ownNames->end())
                        {
                            ownNames->push_back(name);
                        }
                        continue;
                    }
                }
                else if (isVariableDecl(i))
                {
                    return std::unexpected(fmt::format(
                        "{}: variables are file-scope; '{}' cannot be declared inside a rule",
                        located, token.Text));
                }

                // A rule or `@keyframes` statement begins (or continues) here.
                if (depth == 0)
                {
                    seenRule = true;
                }

                // Expand a `var(--name)` use into the emitted stream, at the table's use-site state.
                if (substituted != nullptr && token.Kind == CssTokenKind::Ident &&
                    token.Text == "var" && i + 1 < tokens.size() &&
                    tokens[i + 1].Kind == CssTokenKind::LParen)
                {
                    if (i + 3 >= tokens.size() || tokens[i + 2].Kind != CssTokenKind::Ident ||
                        tokens[i + 3].Kind != CssTokenKind::RParen)
                    {
                        return std::unexpected(
                            fmt::format("{}: malformed var(...); expected 'var(--name)'", located));
                    }
                    const string& name = tokens[i + 2].Text;
                    const auto found = table.find(name);
                    if (found == table.end())
                    {
                        return std::unexpected(
                            fmt::format("{}: use of undefined variable '{}'", located, name));
                    }
                    substituted->insert(substituted->end(), found->second.begin(),
                                        found->second.end());
                    i += 4;
                    continue;
                }

                if (token.Kind == CssTokenKind::LBrace)
                {
                    ++depth;
                }
                else if (token.Kind == CssTokenKind::RBrace && depth > 0)
                {
                    --depth;
                }

                if (substituted != nullptr)
                {
                    substituted->push_back(token);
                }
                ++i;
            }
            return {};
        }
    }

    Result<vector<u8>> StyleSheetImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("stylesheet importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string file = sourcePath.string();

        const std::ifstream in(sourcePath, std::ios::binary);
        if (!in)
        {
            return std::unexpected(
                fmt::format("stylesheet importer: '{}': cannot open source", file));
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        const string source = buffer.str();

        const std::vector<CssToken> rawTokens = TokenizeCss(source);

        // Resolve `@use` imports and file-scope variables, and substitute every `var(--name)` use
        // into the token stream the two-pass rule parse below consumes. `ownVariableNames` records
        // this sheet's own variables for the runtime table; `variableTable` holds their resolved
        // token values.
        StyleVariableTable variableTable;
        vector<string> ownVariableNames;
        std::vector<CssToken> tokens;
        std::set<string> visited;
        {
            std::error_code ec;
            const path canonical = std::filesystem::weakly_canonical(sourcePath, ec);
            visited.insert((ec ? sourcePath : canonical).string());
        }
        const VoidResult preprocessed = PreprocessSheet(
            rawTokens, sourcePath, context, variableTable, &ownVariableNames, &tokens, visited);
        if (!preprocessed)
        {
            return std::unexpected(preprocessed.error());
        }

        vector<CookedStyleRule> rules;
        vector<CookedStyleProperty> properties;
        vector<CookedStyleAnimation> animations;
        vector<CookedStyleKeyframe> keyframes;
        vector<CookedStyleGradient> gradients;
        vector<u8> rampBytes;
        vector<string> animationNames;

        // Pass 1: cook every @keyframes clip first, so a rule may reference a clip declared
        // after it in the source (rules are skipped wholesale here and parsed in pass 2).
        usize i = 0;
        while (i < tokens.size())
        {
            const string located = fmt::format("stylesheet importer: '{}'", file);
            if (IsKeyframesAt(tokens, i))
            {
                const VoidResult parsed = ParseKeyframes(tokens, i, located, properties, animations,
                                                         keyframes, animationNames);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }
            }
            else if (!SkipStatement(tokens, i))
            {
                return std::unexpected(fmt::format("{}: expected '{{' after selector", located));
            }
        }

        // Pass 2: the rules, with the full clip table in hand.
        i = 0;
        while (i < tokens.size())
        {
            const string located = fmt::format("stylesheet importer: '{}'", file);

            if (IsKeyframesAt(tokens, i))
            {
                static_cast<void>(SkipStatement(tokens, i));
                continue;
            }

            // One or more comma-separated selectors, then a shared declaration block.
            vector<ParsedSelector> selectors;
            while (true)
            {
                const Result<ParsedSelector> selector = ParseSelector(tokens, i, located);
                if (!selector)
                {
                    return std::unexpected(selector.error());
                }
                selectors.push_back(*selector);
                if (i < tokens.size() && tokens[i].Kind == CssTokenKind::Comma)
                {
                    ++i;
                    continue;
                }
                break;
            }

            if (i >= tokens.size() || tokens[i].Kind != CssTokenKind::LBrace)
            {
                return std::unexpected(fmt::format("{}: expected '{{' after selector", located));
            }
            ++i; // consume '{'

            const Result<vector<CookedStyleProperty>> block =
                ParseBlock(tokens, i, located, &animationNames, &gradients, &rampBytes);
            if (!block)
            {
                return std::unexpected(block.error());
            }

            // Each selector in the list becomes its own rule sharing a copy of the declarations, so
            // the runtime match is one selector per rule (source order preserved for the cascade).
            for (const ParsedSelector& selector : selectors)
            {
                CookedStyleRule rule{};
                CopyName(rule.Type, StyleSelectorNameCapacity, selector.Type);
                CopyName(rule.Class, StyleSelectorNameCapacity, selector.Class);
                CopyName(rule.Id, StyleSelectorNameCapacity, selector.Id);
                rule.State = static_cast<u32>(selector.State);
                rule.FirstProperty = static_cast<u32>(properties.size());
                rule.PropertyCount = static_cast<u32>(block->size());
                rules.push_back(rule);
                properties.insert(properties.end(), block->begin(), block->end());
            }
        }

        // The runtime variable table: this sheet's own top-level variables whose full value resolves
        // to a color or a single number. A multi-token variable is cook-time-only and gets no entry.
        vector<CookedStyleVariable> variables;
        for (const string& name : ownVariableNames)
        {
            const auto found = variableTable.find(name);
            if (found == variableTable.end())
            {
                continue;
            }
            const string value = AssembleValue(found->second);

            CookedStyleVariable variable{};
            // The name is queried without its leading `--`.
            CopyName(variable.Name, StyleSelectorNameCapacity,
                     name.size() >= 2 ? name.substr(2) : name);

            f32 scalar = 0.0f;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), scalar);
            if (!value.empty() && ec == std::errc{} && ptr == value.data() + value.size())
            {
                variable.Kind = 1; // scalar
                variable.Payload[0] = scalar;
                variables.push_back(variable);
                continue;
            }

            const Result<vec4> color = ParseStyleColor(value, file);
            if (color)
            {
                variable.Kind = 0; // color
                variable.Payload[0] = color->r;
                variable.Payload[1] = color->g;
                variable.Payload[2] = color->b;
                variable.Payload[3] = color->a;
                variables.push_back(variable);
            }
        }

        CookedStyleSheetHeader header{};
        header.Version = CookedStyleSheetVersion;
        header.RuleCount = static_cast<u32>(rules.size());
        header.PropertyCount = static_cast<u32>(properties.size());
        header.AnimationCount = static_cast<u32>(animations.size());
        header.KeyframeCount = static_cast<u32>(keyframes.size());
        header.GradientCount = static_cast<u32>(gradients.size());
        header.VariableCount = static_cast<u32>(variables.size());
        header.RampByteCount = static_cast<u32>(rampBytes.size());

        vector<u8> blob;
        blob.reserve(sizeof(header) + rules.size() * sizeof(CookedStyleRule) +
                     properties.size() * sizeof(CookedStyleProperty) +
                     animations.size() * sizeof(CookedStyleAnimation) +
                     keyframes.size() * sizeof(CookedStyleKeyframe) +
                     gradients.size() * sizeof(CookedStyleGradient) +
                     variables.size() * sizeof(CookedStyleVariable) + rampBytes.size());
        Append(blob, header);
        for (const CookedStyleRule& rule : rules)
        {
            Append(blob, rule);
        }
        for (const CookedStyleProperty& cp : properties)
        {
            Append(blob, cp);
        }
        for (const CookedStyleAnimation& animation : animations)
        {
            Append(blob, animation);
        }
        for (const CookedStyleKeyframe& keyframe : keyframes)
        {
            Append(blob, keyframe);
        }
        for (const CookedStyleGradient& gradient : gradients)
        {
            Append(blob, gradient);
        }
        for (const CookedStyleVariable& variable : variables)
        {
            Append(blob, variable);
        }
        blob.insert(blob.end(), rampBytes.begin(), rampBytes.end());

        return blob;
    }

    void RegisterStyleSheetImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<StyleSheetImporter>());
    }
}
