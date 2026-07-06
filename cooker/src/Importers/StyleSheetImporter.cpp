#include "StyleSheetImporter.h"

#include "CssTokenizer.h"
#include "StyleParse.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

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

        // Parses a declaration block (the tokens between `{` and `}`) into cooked properties.
        Result<vector<CookedStyleProperty>> ParseBlock(const std::vector<CssToken>& tokens,
                                                       usize& i, const string& located)
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

        usize i = 0;
        while (i < tokens.size())
        {
            const string located = fmt::format("stylesheet importer: '{}'", file);

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

            const Result<vector<CookedStyleProperty>> block = ParseBlock(tokens, i, located);
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

        vector<u8> blob;
        blob.reserve(sizeof(header) + rules.size() * sizeof(CookedStyleRule) +
                     properties.size() * sizeof(CookedStyleProperty));
        Append(blob, header);
        for (const CookedStyleRule& rule : rules)
        {
            Append(blob, rule);
        }
        for (const CookedStyleProperty& cp : properties)
        {
            Append(blob, cp);
        }

        return blob;
    }

    void RegisterStyleSheetImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<StyleSheetImporter>());
    }
}
