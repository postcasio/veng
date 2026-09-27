#include <Veng/Localization/Message.h>

#include <fmt/format.h>

namespace Veng::Localization
{
    namespace
    {
        /// @brief Magnitude of a count, so plural arithmetic never depends on sign-dependent
        /// modulo. A count is a quantity; a negative value is treated by its absolute size.
        i64 Magnitude(i64 n)
        {
            return n < 0 ? -n : n;
        }

        // The compiled per-locale plural rules. Each is the CLDR rule for its language group,
        // stated as code. Only integer counts occur here, so the fraction-only categories some
        // languages define are unreachable and omitted.

        /// @brief English/German/Spanish/Italian/Portuguese/Dutch: only the singular is distinct.
        PluralCategory GermanicRomanceRule(i64 n)
        {
            return Magnitude(n) == 1 ? PluralCategory::One : PluralCategory::Other;
        }

        /// @brief French: zero groups with one.
        PluralCategory FrenchRule(i64 n)
        {
            const i64 a = Magnitude(n);
            return (a == 0 || a == 1) ? PluralCategory::One : PluralCategory::Other;
        }

        /// @brief Polish: the three-form rule — the case that proves the mechanism is not
        /// English-shaped.
        PluralCategory PolishRule(i64 n)
        {
            const i64 a = Magnitude(n);
            if (a == 1)
            {
                return PluralCategory::One;
            }
            const i64 m10 = a % 10;
            const i64 m100 = a % 100;
            if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14))
            {
                return PluralCategory::Few;
            }
            return PluralCategory::Many;
        }

        /// @brief Czech/Slovak: one for 1, few for 2..4, other otherwise.
        PluralCategory CzechRule(i64 n)
        {
            const i64 a = Magnitude(n);
            if (a == 1)
            {
                return PluralCategory::One;
            }
            if (a >= 2 && a <= 4)
            {
                return PluralCategory::Few;
            }
            return PluralCategory::Other;
        }

        /// @brief Turkish: no plural distinction.
        PluralCategory NoPluralRule(i64 /*n*/)
        {
            return PluralCategory::Other;
        }

        /// @brief Lowercases an ASCII letter; leaves every other byte unchanged.
        char AsciiLower(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        /// @brief The primary language subtag of a locale id, lowercased — the part before the
        /// first `-`/`_` separator.
        string PrimarySubtag(string_view localeId)
        {
            string out;
            for (const char c : localeId)
            {
                if (c == '-' || c == '_')
                {
                    break;
                }
                out += AsciiLower(c);
            }
            return out;
        }

        /// @brief True for an ASCII decimal digit.
        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        /// @brief Appends a codepoint to a string as UTF-8.
        void AppendUtf8(string& out, char32_t cp)
        {
            const u32 c = static_cast<u32>(cp);
            if (c < 0x80)
            {
                out += static_cast<char>(c);
            }
            else if (c < 0x800)
            {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }

        /// @brief The parsed shape of an fmt replacement spec — enough of the grammar to reject a
        /// spec fmt would throw on for a given argument type.
        ///
        /// The spec grammar parsed is `[[fill]align][sign][#][0][width][.precision][L][type]`.
        /// Dynamic width/precision (`{}` inside the spec) is rejected outright, since it would ask
        /// fmt to resolve an argument this layer does not supply.
        struct ParsedSpec
        {
            /// @brief A sign flag (`+`, `-`, or space) was present.
            bool HasSign = false;
            /// @brief The alternate-form flag (`#`) was present.
            bool HasHash = false;
            /// @brief The zero-pad flag (`0`) was present.
            bool HasZero = false;
            /// @brief A precision (`.N`) was present.
            bool HasPrecision = false;
            /// @brief The presentation type letter, or `\0` when the spec names none.
            char Type = 0;
        };

        /// @brief Parses a replacement spec into @ref ParsedSpec, rejecting a malformed one.
        /// @return True and fills @p out when the spec matches the supported grammar; false when it
        ///         is malformed or uses dynamic width/precision.
        bool ParseSpec(string_view spec, ParsedSpec& out)
        {
            for (const char c : spec)
            {
                if (c == '{' || c == '}')
                {
                    return false;
                }
            }

            const auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };

            usize i = 0;
            const usize n = spec.size();

            // A fill character is only valid immediately before an alignment char.
            if (n >= 2 && isAlign(spec[1]))
            {
                i = 2;
            }
            else if (n >= 1 && isAlign(spec[0]))
            {
                i = 1;
            }

            if (i < n && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' '))
            {
                out.HasSign = true;
                ++i;
            }
            if (i < n && spec[i] == '#')
            {
                out.HasHash = true;
                ++i;
            }
            if (i < n && spec[i] == '0')
            {
                out.HasZero = true;
                ++i;
            }
            while (i < n && IsDigit(spec[i]))
            {
                ++i;
            }
            if (i < n && spec[i] == '.')
            {
                out.HasPrecision = true;
                ++i;
                const usize firstDigit = i;
                while (i < n && IsDigit(spec[i]))
                {
                    ++i;
                }
                if (i == firstDigit)
                {
                    return false;
                }
            }
            if (i < n && spec[i] == 'L')
            {
                ++i;
            }
            if (i < n)
            {
                out.Type = spec[i];
                ++i;
            }
            return i == n;
        }

        /// @brief True for an integer presentation type (or none).
        bool IsIntegerType(char t)
        {
            return t == 0 || t == 'b' || t == 'B' || t == 'c' || t == 'd' || t == 'o' || t == 'x' ||
                   t == 'X' || t == 'n';
        }

        /// @brief True for a floating-point presentation type (or none).
        bool IsFloatType(char t)
        {
            return t == 0 || t == 'a' || t == 'A' || t == 'e' || t == 'E' || t == 'f' || t == 'F' ||
                   t == 'g' || t == 'G' || t == '%';
        }

        /// @brief True when the presentation renders a base-ten number whose digits carry the
        /// locale's decimal and grouping separators; false for a bit/hex/char presentation, which
        /// is passed through unrewritten.
        bool IsDecimalPresentation(char t)
        {
            return t == 0 || t == 'd' || t == 'n' || t == 'f' || t == 'F' || t == 'e' || t == 'E' ||
                   t == 'g' || t == 'G' || t == '%';
        }

        /// @brief Rewrites fmt's C-locale numeric output to a locale's separators: groups the
        /// integer digits and replaces the decimal point.
        ///
        /// The integer part is the first run of ASCII digits, so a leading sign or fill stays put
        /// and an exponent or `%` suffix is left untouched. Grouping is skipped when
        /// @ref NumberFormat::Grouping is @c U'\0'.
        string ApplyNumberFormat(string_view raw, const NumberFormat& numbers)
        {
            usize i = 0;
            const usize n = raw.size();
            while (i < n && !IsDigit(raw[i]))
            {
                ++i;
            }
            const usize digitsBegin = i;
            while (i < n && IsDigit(raw[i]))
            {
                ++i;
            }
            const usize digitsEnd = i;

            string out;
            out.append(raw.substr(0, digitsBegin));

            const usize digitCount = digitsEnd - digitsBegin;
            if (numbers.Grouping != U'\0' && digitCount > 0)
            {
                string separator;
                AppendUtf8(separator, numbers.Grouping);
                for (usize k = 0; k < digitCount; ++k)
                {
                    if (k > 0 && (digitCount - k) % 3 == 0)
                    {
                        out += separator;
                    }
                    out += raw[digitsBegin + k];
                }
            }
            else
            {
                out.append(raw.substr(digitsBegin, digitCount));
            }

            usize rest = digitsEnd;
            if (rest < n && raw[rest] == '.')
            {
                AppendUtf8(out, numbers.Decimal);
                ++rest;
            }
            out.append(raw.substr(rest));
            return out;
        }

        /// @brief Builds the single-field fmt string for a validated spec (`"{}"` or `"{:spec}"`).
        string FieldFormat(string_view spec)
        {
            string field = "{";
            if (!spec.empty())
            {
                field += ':';
                field.append(spec);
            }
            field += '}';
            return field;
        }

        /// @brief Validates a field's spec against its argument type, then formats the value and
        /// appends it, rewriting a numeric result to the locale's separators.
        /// @return An error when the spec is malformed or incompatible with the argument's type;
        ///         success (with @p out extended) otherwise. No path lets fmt see an incompatible
        ///         spec, so fmt never throws.
        VoidResult FormatValue(const ArgValue& value, string_view spec, const NumberFormat& numbers,
                               string& out)
        {
            ParsedSpec parsed;
            if (!spec.empty() && !ParseSpec(spec, parsed))
            {
                return std::unexpected(fmt::format("malformed format spec '{}'", spec));
            }

            switch (value.index())
            {
            case 0:
            {
                const i64 v = std::get<i64>(value);
                if (parsed.HasPrecision || !IsIntegerType(parsed.Type))
                {
                    return std::unexpected(
                        fmt::format("format spec '{}' is not valid for an integer argument", spec));
                }
                const string raw = spec.empty() ? fmt::format("{}", v)
                                                : fmt::format(fmt::runtime(FieldFormat(spec)), v);
                if (IsDecimalPresentation(parsed.Type))
                {
                    out += ApplyNumberFormat(raw, numbers);
                }
                else
                {
                    out += raw;
                }
                return {};
            }
            case 1:
            {
                const f64 v = std::get<f64>(value);
                if (!IsFloatType(parsed.Type))
                {
                    return std::unexpected(fmt::format(
                        "format spec '{}' is not valid for a floating-point argument", spec));
                }
                const string raw = spec.empty() ? fmt::format("{}", v)
                                                : fmt::format(fmt::runtime(FieldFormat(spec)), v);
                out += ApplyNumberFormat(raw, numbers);
                return {};
            }
            case 2:
            {
                const string_view v = std::get<string_view>(value);
                const bool numericFlag = parsed.HasSign || parsed.HasHash || parsed.HasZero;
                if (numericFlag || !(parsed.Type == 0 || parsed.Type == 's'))
                {
                    return std::unexpected(
                        fmt::format("format spec '{}' is not valid for a string argument", spec));
                }
                if (spec.empty())
                {
                    out.append(v);
                }
                else
                {
                    out += fmt::format(fmt::runtime(FieldFormat(spec)), v);
                }
                return {};
            }
            default:
            {
                const bool v = std::get<bool>(value);
                if (parsed.HasPrecision ||
                    !(parsed.Type == 0 || parsed.Type == 's' || IsIntegerType(parsed.Type)))
                {
                    return std::unexpected(
                        fmt::format("format spec '{}' is not valid for a boolean argument", spec));
                }
                out += spec.empty() ? fmt::format("{}", v)
                                    : fmt::format(fmt::runtime(FieldFormat(spec)), v);
                return {};
            }
            }
        }

        /// @brief Decodes the UTF-8 codepoint starting at byte @p i and advances @p i past it.
        ///
        /// A malformed or truncated sequence decodes as its lead byte and advances one byte, so a
        /// bad string is walked to its end rather than stalling.
        char32_t DecodeUtf8(string_view text, usize& i)
        {
            const auto byte = [&](usize k) { return static_cast<u8>(text[k]); };
            const u8 lead = byte(i);
            usize length = 1;
            u32 cp = lead;
            if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                cp = lead & 0x1Fu;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                cp = lead & 0x0Fu;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                cp = lead & 0x07u;
            }
            if (length == 1 || i + length > text.size())
            {
                ++i;
                return static_cast<char32_t>(lead);
            }
            for (usize k = 1; k < length; ++k)
            {
                if ((byte(i + k) & 0xC0) != 0x80)
                {
                    ++i;
                    return static_cast<char32_t>(lead);
                }
                cp = (cp << 6) | (byte(i + k) & 0x3Fu);
            }
            i += length;
            return static_cast<char32_t>(cp);
        }

        /// @brief Lowercases a letter of ASCII, Latin-1 or Latin Extended-A; returns any other
        /// codepoint unchanged.
        char32_t LowerCase(char32_t c)
        {
            const u32 u = static_cast<u32>(c);
            if (u >= 'A' && u <= 'Z')
            {
                return static_cast<char32_t>(u + 0x20);
            }
            if (u >= 0xC0 && u <= 0xDE && u != 0xD7)
            {
                return static_cast<char32_t>(u + 0x20);
            }
            if (u == 0x178)
            {
                return static_cast<char32_t>(0xFF);
            }
            // Latin Extended-A pairs each capital with the next codepoint; the parity of the
            // capital flips at U+0139 and U+014A, and again at U+0179.
            const bool evenCapital = (u >= 0x100 && u <= 0x137) || (u >= 0x14A && u <= 0x177);
            const bool oddCapital = (u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E);
            if ((evenCapital && u % 2 == 0) || (oddCapital && u % 2 == 1))
            {
                return static_cast<char32_t>(u + 1);
            }
            return c;
        }

        /// @brief Capitalizes a letter of ASCII, Latin-1 or Latin Extended-A; returns any other
        /// codepoint unchanged. The inverse of @ref LowerCase.
        char32_t UpperCase(char32_t c)
        {
            const u32 u = static_cast<u32>(c);
            if (u >= 'a' && u <= 'z')
            {
                return static_cast<char32_t>(u - 0x20);
            }
            if (u >= 0xE0 && u <= 0xFE && u != 0xF7)
            {
                return static_cast<char32_t>(u - 0x20);
            }
            if (u == 0xFF)
            {
                return static_cast<char32_t>(0x178);
            }
            const bool oddSmall = (u >= 0x101 && u <= 0x137) || (u >= 0x14B && u <= 0x177);
            const bool evenSmall = (u >= 0x13A && u <= 0x148) || (u >= 0x17A && u <= 0x17E);
            if ((oddSmall && u % 2 == 1) || (evenSmall && u % 2 == 0))
            {
                return static_cast<char32_t>(u - 1);
            }
            return c;
        }

        /// @brief True for a capital letter — one @ref LowerCase changes.
        bool IsCapital(char32_t c)
        {
            return LowerCase(c) != c;
        }

        /// @brief True for a codepoint that belongs to a word: an ASCII letter, or anything from
        /// U+00C0 up other than the Latin-1 operators and the general and CJK punctuation blocks.
        bool IsWordLetter(char32_t c)
        {
            const u32 u = static_cast<u32>(c);
            if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z'))
            {
                return true;
            }
            if (u < 0xC0 || u == 0xD7 || u == 0xF7)
            {
                return false;
            }
            return !(u >= 0x2000 && u <= 0x206F) && !(u >= 0x3000 && u <= 0x303F);
        }

        /// @brief Whether two words are equal ignoring case.
        bool EqualIgnoringCase(string_view a, string_view b)
        {
            usize i = 0;
            usize j = 0;
            while (i < a.size() && j < b.size())
            {
                if (LowerCase(DecodeUtf8(a, i)) != LowerCase(DecodeUtf8(b, j)))
                {
                    return false;
                }
            }
            return i == a.size() && j == b.size();
        }

        /// @brief Whether a rendered value begins with a vowel sound under a table: its first
        /// letter is one of the table's initials and its first word is not an all-capital
        /// initialism of two or more letters.
        bool BeginsWithVowelSound(string_view value, const ElisionTable& table)
        {
            if (value.empty())
            {
                return false;
            }
            usize i = 0;
            const char32_t first = DecodeUtf8(value, i);
            if (!IsWordLetter(first))
            {
                return false;
            }
            const char32_t folded = LowerCase(first);
            bool listed = false;
            for (usize k = 0; k < table.Initials.size() && !listed;)
            {
                listed = LowerCase(DecodeUtf8(table.Initials, k)) == folded;
            }
            if (!listed)
            {
                return false;
            }

            usize letters = 1;
            bool allCapitals = IsCapital(first);
            while (i < value.size() && allCapitals)
            {
                const char32_t next = DecodeUtf8(value, i);
                if (!IsWordLetter(next))
                {
                    break;
                }
                ++letters;
                allCapitals = IsCapital(next);
            }
            return !(allCapitals && letters >= 2);
        }

        /// @brief The elided form as the written word's case asks: all capitals for a word of two
        /// or more capitals, a leading capital for a capitalized word, and as authored otherwise.
        string MatchCase(string_view written, string_view elided)
        {
            usize letters = 0;
            usize capitals = 0;
            bool firstCapital = false;
            for (usize i = 0; i < written.size();)
            {
                const char32_t c = DecodeUtf8(written, i);
                firstCapital = firstCapital || (letters == 0 && IsCapital(c));
                ++letters;
                capitals += IsCapital(c) ? 1 : 0;
            }
            const bool allCapitals = letters >= 2 && capitals == letters;
            if (!allCapitals && !firstCapital)
            {
                return string(elided);
            }

            string out;
            bool capitalized = false;
            for (usize i = 0; i < elided.size();)
            {
                const char32_t c = DecodeUtf8(elided, i);
                const bool hasCase = IsCapital(UpperCase(c));
                const bool raise = allCapitals || (!capitalized && hasCase);
                AppendUtf8(out, raise ? UpperCase(c) : c);
                capitalized = capitalized || hasCase;
            }
            return out;
        }

        /// @brief Elides the word before a just-formatted field when the table says it elides.
        ///
        /// @p out holds the formatted text so far: the literal template text of this field's run
        /// starts at @p literalBegin and the field's rendered value at @p valueBegin. The word must
        /// lie wholly within that literal run, stand as a whole word, and be followed by exactly one
        /// ASCII space; the value must begin with a vowel sound (@ref BeginsWithVowelSound).
        void ApplyElision(string& out, usize literalBegin, usize valueBegin,
                          const ElisionTable& table)
        {
            if (table.Empty() || valueBegin < literalBegin + 2 || out[valueBegin - 1] != ' ')
            {
                return;
            }
            if (!BeginsWithVowelSound(string_view(out).substr(valueBegin), table))
            {
                return;
            }

            // Walk back over the word's letters, codepoint by codepoint.
            const usize wordEnd = valueBegin - 1;
            usize wordBegin = wordEnd;
            while (wordBegin > 0)
            {
                usize lead = wordBegin - 1;
                while (lead > 0 && (static_cast<u8>(out[lead]) & 0xC0) == 0x80)
                {
                    --lead;
                }
                usize cursor = lead;
                if (!IsWordLetter(DecodeUtf8(out, cursor)))
                {
                    break;
                }
                wordBegin = lead;
            }
            if (wordBegin == wordEnd || wordBegin < literalBegin)
            {
                return;
            }

            const string_view written = string_view(out).substr(wordBegin, wordEnd - wordBegin);
            for (const ElisionRule& rule : table.Rules)
            {
                if (EqualIgnoringCase(written, rule.Word))
                {
                    const string value = out.substr(valueBegin);
                    const string elided = MatchCase(written, rule.Elided);
                    out.resize(wordBegin);
                    out += elided;
                    out += value;
                    return;
                }
            }
        }

        /// @brief Selects the template a formatting run will use — the sole template, or the plural
        /// variant for the count, falling back to the @ref PluralCategory::Other slot.
        const string& SelectTemplate(const Message& message, optional<i64> count, PluralRule rule)
        {
            if (!message.Plurals.has_value())
            {
                return message.Template;
            }
            const std::array<string, PluralCategoryCount>& variants = *message.Plurals;
            PluralCategory category = PluralCategory::Other;
            if (count.has_value() && rule != nullptr)
            {
                category = rule(*count);
            }
            const string& chosen = variants[static_cast<usize>(category)];
            return chosen.empty() ? variants[static_cast<usize>(PluralCategory::Other)] : chosen;
        }
    }

    PluralRule PluralRuleFor(string_view localeId)
    {
        const string tag = PrimarySubtag(localeId);
        if (tag == "fr")
        {
            return &FrenchRule;
        }
        if (tag == "pl")
        {
            return &PolishRule;
        }
        if (tag == "cs" || tag == "sk")
        {
            return &CzechRule;
        }
        if (tag == "tr")
        {
            return &NoPluralRule;
        }
        // English and the Germanic/Romance languages that share its rule, plus every unknown id.
        return &GermanicRomanceRule;
    }

    Result<string> FormatMessage(const Message& message, std::span<const FormatArg> args,
                                 optional<i64> count, PluralRule rule, NumberFormat numbers,
                                 const ElisionTable& elision)
    {
        const string& tmpl = SelectTemplate(message, count, rule);

        string out;
        out.reserve(tmpl.size());

        // Where the literal template text since the last field begins in `out`: an elided word
        // must lie within it, so substituted text never elides.
        usize literalBegin = 0;

        usize i = 0;
        const usize n = tmpl.size();
        while (i < n)
        {
            const char c = tmpl[i];
            if (c == '{')
            {
                if (i + 1 < n && tmpl[i + 1] == '{')
                {
                    out += '{';
                    i += 2;
                    continue;
                }
                const usize close = tmpl.find('}', i + 1);
                if (close == string::npos)
                {
                    return std::unexpected(string("unbalanced '{' in message template"));
                }
                const string_view field = string_view(tmpl).substr(i + 1, close - (i + 1));
                const usize colon = field.find(':');
                const string_view name =
                    colon == string_view::npos ? field : field.substr(0, colon);
                const string_view spec =
                    colon == string_view::npos ? string_view{} : field.substr(colon + 1);

                const usize valueBegin = out.size();
                if (name == "#")
                {
                    if (!count.has_value())
                    {
                        return std::unexpected(
                            string("message uses '{#}' but no count was provided"));
                    }
                    const ArgValue countValue = static_cast<i64>(*count);
                    const VoidResult formatted = FormatValue(countValue, spec, numbers, out);
                    if (!formatted)
                    {
                        return std::unexpected(formatted.error());
                    }
                }
                else
                {
                    const FormatArg* found = nullptr;
                    for (const FormatArg& arg : args)
                    {
                        if (arg.Name == name)
                        {
                            found = &arg;
                            break;
                        }
                    }
                    if (found == nullptr)
                    {
                        return std::unexpected(
                            fmt::format("unbound field '{}' in message template", name));
                    }
                    const VoidResult formatted = FormatValue(found->Value, spec, numbers, out);
                    if (!formatted)
                    {
                        return std::unexpected(formatted.error());
                    }
                }
                ApplyElision(out, literalBegin, valueBegin, elision);
                literalBegin = out.size();
                i = close + 1;
            }
            else if (c == '}')
            {
                if (i + 1 < n && tmpl[i + 1] == '}')
                {
                    out += '}';
                    i += 2;
                    continue;
                }
                return std::unexpected(string("unmatched '}' in message template"));
            }
            else
            {
                out += c;
                ++i;
            }
        }

        return out;
    }
}
