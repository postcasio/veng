#pragma once

#include <Veng/Veng.h>

#include <string_view>
#include <vector>

namespace Veng::Cook
{
    /// @brief The kind of one lexed USS token.
    ///
    /// A deliberately small set for the USS subset: the selector punctuation (`.` `#` `:` `,`),
    /// the block delimiters (`{` `}`), the declaration delimiters (`;` `:`), and the two content
    /// tokens (an identifier, and any other run of value characters). Whitespace and `/* */`
    /// comments are skipped by the tokenizer and never emitted.
    enum class CssTokenKind : u8
    {
        /// @brief A `.` introducing a class selector.
        Dot,
        /// @brief A `#` introducing an id selector.
        Hash,
        /// @brief A `:` — a pseudo-state in a selector or the property/value separator in a declaration.
        Colon,
        /// @brief A `,` separating selectors in a selector list.
        Comma,
        /// @brief A `;` terminating a declaration.
        Semicolon,
        /// @brief A `{` opening a declaration block.
        LBrace,
        /// @brief A `}` closing a declaration block.
        RBrace,
        /// @brief An identifier: a letter/underscore/hyphen run (a type name, class, id, property, or keyword).
        Ident,
        /// @brief Any other value run (a `#rrggbb` color, a number, a `12px` length) up to a delimiter.
        Value,
    };

    /// @brief One lexed USS token: its kind and its source text.
    struct CssToken
    {
        /// @brief The token's kind.
        CssTokenKind Kind = CssTokenKind::Ident;
        /// @brief The token's source text (empty for punctuation tokens).
        string Text;
    };

    /// @brief Lexes a `*.vuss` source string into a flat token stream.
    ///
    /// Skips whitespace and `/* */` comments, emitting the selector punctuation, block/declaration
    /// delimiters, identifiers, and value runs the USS grammar needs. It never fails — malformed
    /// structure surfaces as a located error in the parser that consumes the stream, not here. A
    /// `#` immediately followed by a hex color in a value position is still lexed as a Hash then a
    /// Value; the declaration parser recombines `#` + hex into a color, while a selector parser
    /// reads `#` + Ident as an id selector.
    /// @param source  The USS source text.
    /// @return The lexed tokens, in source order.
    [[nodiscard]] std::vector<CssToken> TokenizeCss(std::string_view source);
}
