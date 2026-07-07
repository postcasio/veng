#include "StyleSheetImporter.h"

#include "CssTokenizer.h"
#include "StyleParse.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Gui/Element.h>
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

        // Parses a declaration block (the tokens between `{` and `}`) into cooked properties.
        // `animationNames` is the sheet's @keyframes table an `animation` declaration resolves
        // against; nullptr where an animation reference is not authorable (a keyframe block).
        Result<vector<CookedStyleProperty>> ParseBlock(const std::vector<CssToken>& tokens,
                                                       usize& i, const string& located,
                                                       const vector<string>* animationNames)
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
                        if (!value.empty() && value.back() != '#')
                        {
                            value += ' ';
                        }
                        value += tokens[i].Text;
                        break;
                    case CssTokenKind::Dot:
                        value += '.';
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
                    ParseBlock(tokens, i, located, nullptr);
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

        const std::vector<CssToken> tokens = TokenizeCss(source);

        vector<CookedStyleRule> rules;
        vector<CookedStyleProperty> properties;
        vector<CookedStyleAnimation> animations;
        vector<CookedStyleKeyframe> keyframes;
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
                ParseBlock(tokens, i, located, &animationNames);
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

        CookedStyleSheetHeader header{};
        header.Version = CookedStyleSheetVersion;
        header.RuleCount = static_cast<u32>(rules.size());
        header.PropertyCount = static_cast<u32>(properties.size());
        header.AnimationCount = static_cast<u32>(animations.size());
        header.KeyframeCount = static_cast<u32>(keyframes.size());

        vector<u8> blob;
        blob.reserve(sizeof(header) + rules.size() * sizeof(CookedStyleRule) +
                     properties.size() * sizeof(CookedStyleProperty) +
                     animations.size() * sizeof(CookedStyleAnimation) +
                     keyframes.size() * sizeof(CookedStyleKeyframe));
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

        return blob;
    }

    void RegisterStyleSheetImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<StyleSheetImporter>());
    }
}
