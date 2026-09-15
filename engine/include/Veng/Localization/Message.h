#pragma once

#include <array>
#include <span>
#include <variant>

#include <Veng/Result.h>
#include <Veng/Veng.h>

/// @brief The device-free message-format layer: named placeholders, CLDR plural selection, and
/// locale number separators over a small, exception-free subset of fmt.
///
/// A translated string is rarely a fixed literal. It carries runtime values (a distance, a
/// count, a name) and its grammar depends on those values — "1 jump left" against "3 jumps
/// left", and a third form again in a language like Polish. This layer turns a message template
/// plus named arguments plus an optional count into a finished string, correctly for a target
/// locale's plural rules and numeric presentation.
///
/// Two properties are load-bearing. Replacement fields are named (`"{name} is {dist} away"`), so
/// a translator reorders them freely and the arguments still bind by name. And formatting never
/// lets fmt throw: the template is parsed and validated here before any value is formatted, so a
/// malformed template or a spec/type mismatch returns a Result error rather than raising
/// `fmt::format_error` — which, crossing veng's `-fno-exceptions` boundary, would terminate the
/// process. Everything here is CPU-side, device-free, and allocates only the result string; it
/// names no GUI, asset, or renderer type, so it links into the cooker and unit tests with no
/// engine bring-up.
namespace Veng::Localization
{
    /// @brief The plural categories the CLDR distinguishes across languages.
    ///
    /// A language selects one category for a given count through its plural rule; the message
    /// carries a variant string per category it uses. No target language needs all six, but the
    /// set is fixed so a variant array has one slot per category regardless of language.
    enum class PluralCategory : u8
    {
        Zero,
        One,
        Two,
        Few,
        Many,
        Other
    };

    /// @brief Number of plural categories — the width of a message's plural-variant array.
    inline constexpr usize PluralCategoryCount = 6;

    /// @brief A translatable message: a single template, or a set of count-varying plural variants.
    ///
    /// When @ref Plurals is unset the message is @ref Template, formatted directly and ignoring any
    /// count. When @ref Plurals is set, the variant for the count's selected @ref PluralCategory is
    /// used; a variant left empty falls back to the @ref PluralCategory::Other slot, which every
    /// pluralized message defines.
    struct Message
    {
        /// @brief The sole template, used when the message is not pluralized.
        string Template;

        /// @brief The per-category plural variants, indexed by @ref PluralCategory, when the
        /// message varies by count.
        optional<std::array<string, PluralCategoryCount>> Plurals;
    };

    /// @brief A display value substituted into a message field.
    ///
    /// The set is closed on purpose: a message argument is a value shown to a player — a number, a
    /// piece of text, or a flag — never an arbitrary object. A `f64` accepts an fmt precision/type
    /// spec through its field (`{dist:.1f}`); an `i64` and `f64` are both rewritten to the locale's
    /// number separators as they are substituted.
    using ArgValue = std::variant<i64, f64, string_view, bool>;

    /// @brief A named argument bound to a message's replacement field.
    struct FormatArg
    {
        /// @brief The field name this argument binds to, matched against `{name}` in the template.
        string_view Name;

        /// @brief The value substituted where the field appears.
        ArgValue Value;
    };

    /// @brief The locale's numeric presentation — the separators a formatted number uses.
    ///
    /// A number is formatted through fmt in the C locale, then its decimal point and thousands
    /// separators are rewritten to these. The target Latin-script European languages all use a
    /// decimal comma, so `1.5` renders `1,5` and `12000` renders `12 000` under the right
    /// separators. The defaults are English.
    struct NumberFormat
    {
        /// @brief The decimal separator — `.` in English, `,` in French/German/Spanish/Italian/…
        char32_t Decimal = U'.';

        /// @brief The thousands (grouping) separator; @c U'\0' disables grouping entirely.
        char32_t Grouping = U',';
    };

    /// @brief A locale's plural rule: maps a count to its CLDR plural category.
    ///
    /// A plural rule is code — a branch on arithmetic — not translatable data, so the supported
    /// locales' rules are a compiled-in table reached through @ref PluralRuleFor rather than a
    /// per-catalog field.
    using PluralRule = PluralCategory (*)(i64);

    /// @brief Returns the compiled plural rule for a BCP-47-ish locale id.
    ///
    /// Only the primary language subtag is significant (`"en-US"` resolves as `"en"`), matched
    /// case-insensitively. An unrecognized id falls back to the English rule (`n == 1 → One`,
    /// otherwise `Other`); the missing-catalog case is handled separately by the locale fallback
    /// chain, not here.
    /// @param localeId  A locale id such as `"en"`, `"fr"`, `"pl-PL"`.
    /// @return The plural rule for the locale, or the English rule when the id is unknown.
    PluralRule PluralRuleFor(string_view localeId);

    /// @brief Formats a message against named arguments, selecting the plural variant for a count.
    ///
    /// The template is parsed here and each field validated before any value is formatted: a name
    /// is resolved against @p args (the distinguished field `{#}` resolves to @p count), and a
    /// field's fmt spec is checked for compatibility with its argument's type. Only a validated
    /// field is handed to fmt, one value at a time, so fmt is never given an unbound name or an
    /// incompatible spec and never throws. Numeric arguments are rewritten to @p numbers's
    /// separators as they are substituted. A literal brace is written `{{` or `}}`.
    ///
    /// @param message  The message to format — a template or a set of plural variants.
    /// @param args     The named arguments bound to the template's fields.
    /// @param count    The selecting count; drives plural selection and binds `{#}`. When the
    ///                 message is pluralized and this is empty, the @ref PluralCategory::Other
    ///                 variant is used.
    /// @param rule     The plural rule selecting a variant from @p count (see @ref PluralRuleFor).
    /// @param numbers  The locale's number separators applied to numeric arguments.
    /// @return The formatted string, or an error describing the first malformed field — an unbound
    ///         name, an unbalanced brace, or a spec incompatible with its argument's type.
    Result<string> FormatMessage(const Message& message, std::span<const FormatArg> args,
                                 optional<i64> count, PluralRule rule, NumberFormat numbers = {});
}
