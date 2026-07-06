#include "CssTokenizer.h"

#include <cctype>

namespace Veng::Cook
{
    namespace
    {
        bool IsIdentStart(char c)
        {
            return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
        }

        bool IsIdentChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
        }

        // A value run is any non-delimiter, non-whitespace character (a number, a length like 12px,
        // a hex color body, a percentage). It stops at the USS punctuation the grammar reserves.
        bool IsValueChar(char c)
        {
            switch (c)
            {
            case '.':
            case '#':
            case ':':
            case ',':
            case ';':
            case '{':
            case '}':
                return false;
            default:
                return std::isspace(static_cast<unsigned char>(c)) == 0;
            }
        }
    }

    std::vector<CssToken> TokenizeCss(std::string_view source)
    {
        std::vector<CssToken> tokens;
        usize i = 0;
        const usize n = source.size();

        while (i < n)
        {
            const char c = source[i];

            if (std::isspace(static_cast<unsigned char>(c)) != 0)
            {
                ++i;
                continue;
            }

            // A `/* ... */` block comment is skipped wholesale (an unterminated one runs to EOF).
            if (c == '/' && i + 1 < n && source[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/'))
                {
                    ++i;
                }
                i = i + 2 < n ? i + 2 : n;
                continue;
            }

            // A '.' immediately followed by a digit is a decimal point inside a number (0.5), so it
            // starts/continues a value run rather than a class-selector dot.
            if (c == '.' && i + 1 < n &&
                std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0)
            {
                const usize start = i;
                ++i;
                while (i < n && (IsValueChar(source[i]) ||
                                 (source[i] == '.' && i + 1 < n &&
                                  std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0)))
                {
                    ++i;
                }
                tokens.push_back(
                    {.Kind = CssTokenKind::Value, .Text = string(source.substr(start, i - start))});
                continue;
            }

            switch (c)
            {
            case '.':
                tokens.push_back({.Kind = CssTokenKind::Dot});
                ++i;
                continue;
            case '#':
                tokens.push_back({.Kind = CssTokenKind::Hash});
                ++i;
                continue;
            case ':':
                tokens.push_back({.Kind = CssTokenKind::Colon});
                ++i;
                continue;
            case ',':
                tokens.push_back({.Kind = CssTokenKind::Comma});
                ++i;
                continue;
            case ';':
                tokens.push_back({.Kind = CssTokenKind::Semicolon});
                ++i;
                continue;
            case '{':
                tokens.push_back({.Kind = CssTokenKind::LBrace});
                ++i;
                continue;
            case '}':
                tokens.push_back({.Kind = CssTokenKind::RBrace});
                ++i;
                continue;
            default:
                break;
            }

            if (IsIdentStart(c))
            {
                const usize start = i;
                while (i < n && IsIdentChar(source[i]))
                {
                    ++i;
                }
                tokens.push_back(
                    {.Kind = CssTokenKind::Ident, .Text = string(source.substr(start, i - start))});
                continue;
            }

            const usize start = i;
            while (i < n && (IsValueChar(source[i]) ||
                             (source[i] == '.' && i + 1 < n &&
                              std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0)))
            {
                ++i;
            }
            if (i == start)
            {
                // A stray reserved character not handled above (should not occur); skip it.
                ++i;
                continue;
            }
            tokens.push_back(
                {.Kind = CssTokenKind::Value, .Text = string(source.substr(start, i - start))});
        }

        return tokens;
    }
}
