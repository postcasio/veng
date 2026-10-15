#pragma once

#include <Veng/Veng.h>

/// @brief Unicode case mapping for the scripts the engine's text path draws: Latin, Greek and
/// Cyrillic.
///
/// One mapper serves every consumer that changes a string's case — the GUI's `text-transform`, the
/// message formatter's case matching — so two places that upper-case the same string cannot
/// disagree about it.
///
/// The codepoint functions are the Unicode **simple** case mappings (UnicodeData.txt) of every
/// codepoint that maps into or out of U+0000–U+04FF: Basic Latin, Latin-1 Supplement, Latin
/// Extended-A and -B, IPA Extensions, Greek and Coptic, and Cyrillic. A codepoint with no mapping
/// there — a digit, punctuation, CJK, any other script — maps to itself.
///
/// The string functions apply what a codepoint cannot express: upper-casing expands a lower-case
/// letter that has no single capital (`ß` → `SS`, `ŉ` → `ʼN`, `ǰ`, `ΐ`, `ΰ`), and lower-casing
/// writes a capital sigma that ends a word as the final form `ς`. They are locale-independent: no
/// Turkish dotted-I or Lithuanian rule applies. Both take UTF-8 and pass an ill-formed byte
/// through unchanged, so a bad string comes out exactly as bad as it went in.
namespace Veng::Text
{
    /// @brief The simple upper-case mapping of a codepoint.
    /// @param c  The codepoint.
    /// @return Its capital, or @p c itself when it has none.
    [[nodiscard]] VE_API char32_t UpperCase(char32_t c);

    /// @brief The simple lower-case mapping of a codepoint.
    /// @param c  The codepoint.
    /// @return Its small form, or @p c itself when it has none.
    [[nodiscard]] VE_API char32_t LowerCase(char32_t c);

    /// @brief Upper-cases a UTF-8 string, expanding the letters with no single capital.
    /// @param text  UTF-8 text; ill-formed bytes are copied through unchanged.
    /// @return The upper-cased text, which may be longer than @p text.
    [[nodiscard]] VE_API string UpperCase(string_view text);

    /// @brief Lower-cases a UTF-8 string, writing a word-final capital sigma as `ς`.
    /// @param text  UTF-8 text; ill-formed bytes are copied through unchanged.
    /// @return The lower-cased text.
    [[nodiscard]] VE_API string LowerCase(string_view text);
}
