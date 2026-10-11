#include <Veng/Text/CaseMapping.h>

#include <algorithm>
#include <array>
#include <span>

namespace Veng::Text
{
    namespace
    {
        /// @brief A run of codepoints sharing one case mapping: every @ref Stride -th codepoint
        /// from @ref First to @ref Last maps to itself plus @ref Delta.
        struct CaseRun
        {
            /// @brief The run's first source codepoint.
            u32 First;
            /// @brief The run's last source codepoint.
            u32 Last;
            /// @brief The spacing of the run's sources: 1 for a block, 2 for alternating pairs.
            u32 Stride;
            /// @brief What each source adds to reach its mapping.
            i32 Delta;
        };

        /// @brief A lower-case letter whose capital is more than one codepoint.
        struct UpperExpansion
        {
            /// @brief The lower-case letter.
            u32 Source;
            /// @brief Its upper-case spelling, @ref Length codepoints of it.
            std::array<u32, 3> Upper;
            /// @brief How many of @ref Upper are used.
            u32 Length;
        };

        /// @brief An inclusive codepoint range.
        struct CodepointRange
        {
            /// @brief The first codepoint in the range.
            u32 First;
            /// @brief The last codepoint in the range.
            u32 Last;
        };

        // The tables follow Unicode 17.0.0 (UnicodeData.txt and SpecialCasing.txt) and are
        // produced by scripts/generate_case_tables.py, which states the coverage rule.
        // BEGIN GENERATED
        constexpr std::array<CaseRun, 125> UpperRuns{{
            {0x0061, 0x007A, 1, -32},    {0x00B5, 0x00B5, 1, 743},    {0x00E0, 0x00F6, 1, -32},
            {0x00F8, 0x00FE, 1, -32},    {0x00FF, 0x00FF, 1, 121},    {0x0101, 0x012F, 2, -1},
            {0x0131, 0x0131, 1, -232},   {0x0133, 0x0137, 2, -1},     {0x013A, 0x0148, 2, -1},
            {0x014B, 0x0177, 2, -1},     {0x017A, 0x017E, 2, -1},     {0x017F, 0x017F, 1, -300},
            {0x0180, 0x0180, 1, 195},    {0x0183, 0x0185, 2, -1},     {0x0188, 0x0188, 1, -1},
            {0x018C, 0x018C, 1, -1},     {0x0192, 0x0192, 1, -1},     {0x0195, 0x0195, 1, 97},
            {0x0199, 0x0199, 1, -1},     {0x019A, 0x019A, 1, 163},    {0x019B, 0x019B, 1, 42561},
            {0x019E, 0x019E, 1, 130},    {0x01A1, 0x01A5, 2, -1},     {0x01A8, 0x01A8, 1, -1},
            {0x01AD, 0x01AD, 1, -1},     {0x01B0, 0x01B0, 1, -1},     {0x01B4, 0x01B6, 2, -1},
            {0x01B9, 0x01B9, 1, -1},     {0x01BD, 0x01BD, 1, -1},     {0x01BF, 0x01BF, 1, 56},
            {0x01C5, 0x01C5, 1, -1},     {0x01C6, 0x01C6, 1, -2},     {0x01C8, 0x01C8, 1, -1},
            {0x01C9, 0x01C9, 1, -2},     {0x01CB, 0x01CB, 1, -1},     {0x01CC, 0x01CC, 1, -2},
            {0x01CE, 0x01DC, 2, -1},     {0x01DD, 0x01DD, 1, -79},    {0x01DF, 0x01EF, 2, -1},
            {0x01F2, 0x01F2, 1, -1},     {0x01F3, 0x01F3, 1, -2},     {0x01F5, 0x01F5, 1, -1},
            {0x01F9, 0x021F, 2, -1},     {0x0223, 0x0233, 2, -1},     {0x023C, 0x023C, 1, -1},
            {0x023F, 0x0240, 1, 10815},  {0x0242, 0x0242, 1, -1},     {0x0247, 0x024F, 2, -1},
            {0x0250, 0x0250, 1, 10783},  {0x0251, 0x0251, 1, 10780},  {0x0252, 0x0252, 1, 10782},
            {0x0253, 0x0253, 1, -210},   {0x0254, 0x0254, 1, -206},   {0x0256, 0x0257, 1, -205},
            {0x0259, 0x0259, 1, -202},   {0x025B, 0x025B, 1, -203},   {0x025C, 0x025C, 1, 42319},
            {0x0260, 0x0260, 1, -205},   {0x0261, 0x0261, 1, 42315},  {0x0263, 0x0263, 1, -207},
            {0x0264, 0x0264, 1, 42343},  {0x0265, 0x0265, 1, 42280},  {0x0266, 0x0266, 1, 42308},
            {0x0268, 0x0268, 1, -209},   {0x0269, 0x0269, 1, -211},   {0x026A, 0x026A, 1, 42308},
            {0x026B, 0x026B, 1, 10743},  {0x026C, 0x026C, 1, 42305},  {0x026F, 0x026F, 1, -211},
            {0x0271, 0x0271, 1, 10749},  {0x0272, 0x0272, 1, -213},   {0x0275, 0x0275, 1, -214},
            {0x027D, 0x027D, 1, 10727},  {0x0280, 0x0280, 1, -218},   {0x0282, 0x0282, 1, 42307},
            {0x0283, 0x0283, 1, -218},   {0x0287, 0x0287, 1, 42282},  {0x0288, 0x0288, 1, -218},
            {0x0289, 0x0289, 1, -69},    {0x028A, 0x028B, 1, -217},   {0x028C, 0x028C, 1, -71},
            {0x0292, 0x0292, 1, -219},   {0x029D, 0x029D, 1, 42261},  {0x029E, 0x029E, 1, 42258},
            {0x0345, 0x0345, 1, 84},     {0x0371, 0x0373, 2, -1},     {0x0377, 0x0377, 1, -1},
            {0x037B, 0x037D, 1, 130},    {0x03AC, 0x03AC, 1, -38},    {0x03AD, 0x03AF, 1, -37},
            {0x03B1, 0x03C1, 1, -32},    {0x03C2, 0x03C2, 1, -31},    {0x03C3, 0x03CB, 1, -32},
            {0x03CC, 0x03CC, 1, -64},    {0x03CD, 0x03CE, 1, -63},    {0x03D0, 0x03D0, 1, -62},
            {0x03D1, 0x03D1, 1, -57},    {0x03D5, 0x03D5, 1, -47},    {0x03D6, 0x03D6, 1, -54},
            {0x03D7, 0x03D7, 1, -8},     {0x03D9, 0x03EF, 2, -1},     {0x03F0, 0x03F0, 1, -86},
            {0x03F1, 0x03F1, 1, -80},    {0x03F2, 0x03F2, 1, 7},      {0x03F3, 0x03F3, 1, -116},
            {0x03F5, 0x03F5, 1, -96},    {0x03F8, 0x03F8, 1, -1},     {0x03FB, 0x03FB, 1, -1},
            {0x0430, 0x044F, 1, -32},    {0x0450, 0x045F, 1, -80},    {0x0461, 0x0481, 2, -1},
            {0x048B, 0x04BF, 2, -1},     {0x04C2, 0x04CE, 2, -1},     {0x04CF, 0x04CF, 1, -15},
            {0x04D1, 0x04FF, 2, -1},     {0x1C80, 0x1C80, 1, -6254},  {0x1C81, 0x1C81, 1, -6253},
            {0x1C82, 0x1C82, 1, -6244},  {0x1C83, 0x1C84, 1, -6242},  {0x1C85, 0x1C85, 1, -6243},
            {0x1C86, 0x1C86, 1, -6236},  {0x1C87, 0x1C87, 1, -6181},  {0x1FBE, 0x1FBE, 1, -7205},
            {0x2C65, 0x2C65, 1, -10795}, {0x2C66, 0x2C66, 1, -10792},
        }};

        constexpr std::array<CaseRun, 109> LowerRuns{{
            {0x0041, 0x005A, 1, 32},     {0x00C0, 0x00D6, 1, 32},     {0x00D8, 0x00DE, 1, 32},
            {0x0100, 0x012E, 2, 1},      {0x0130, 0x0130, 1, -199},   {0x0132, 0x0136, 2, 1},
            {0x0139, 0x0147, 2, 1},      {0x014A, 0x0176, 2, 1},      {0x0178, 0x0178, 1, -121},
            {0x0179, 0x017D, 2, 1},      {0x0181, 0x0181, 1, 210},    {0x0182, 0x0184, 2, 1},
            {0x0186, 0x0186, 1, 206},    {0x0187, 0x0187, 1, 1},      {0x0189, 0x018A, 1, 205},
            {0x018B, 0x018B, 1, 1},      {0x018E, 0x018E, 1, 79},     {0x018F, 0x018F, 1, 202},
            {0x0190, 0x0190, 1, 203},    {0x0191, 0x0191, 1, 1},      {0x0193, 0x0193, 1, 205},
            {0x0194, 0x0194, 1, 207},    {0x0196, 0x0196, 1, 211},    {0x0197, 0x0197, 1, 209},
            {0x0198, 0x0198, 1, 1},      {0x019C, 0x019C, 1, 211},    {0x019D, 0x019D, 1, 213},
            {0x019F, 0x019F, 1, 214},    {0x01A0, 0x01A4, 2, 1},      {0x01A6, 0x01A6, 1, 218},
            {0x01A7, 0x01A7, 1, 1},      {0x01A9, 0x01A9, 1, 218},    {0x01AC, 0x01AC, 1, 1},
            {0x01AE, 0x01AE, 1, 218},    {0x01AF, 0x01AF, 1, 1},      {0x01B1, 0x01B2, 1, 217},
            {0x01B3, 0x01B5, 2, 1},      {0x01B7, 0x01B7, 1, 219},    {0x01B8, 0x01B8, 1, 1},
            {0x01BC, 0x01BC, 1, 1},      {0x01C4, 0x01C4, 1, 2},      {0x01C5, 0x01C5, 1, 1},
            {0x01C7, 0x01C7, 1, 2},      {0x01C8, 0x01C8, 1, 1},      {0x01CA, 0x01CA, 1, 2},
            {0x01CB, 0x01DB, 2, 1},      {0x01DE, 0x01EE, 2, 1},      {0x01F1, 0x01F1, 1, 2},
            {0x01F2, 0x01F4, 2, 1},      {0x01F6, 0x01F6, 1, -97},    {0x01F7, 0x01F7, 1, -56},
            {0x01F8, 0x021E, 2, 1},      {0x0220, 0x0220, 1, -130},   {0x0222, 0x0232, 2, 1},
            {0x023A, 0x023A, 1, 10795},  {0x023B, 0x023B, 1, 1},      {0x023D, 0x023D, 1, -163},
            {0x023E, 0x023E, 1, 10792},  {0x0241, 0x0241, 1, 1},      {0x0243, 0x0243, 1, -195},
            {0x0244, 0x0244, 1, 69},     {0x0245, 0x0245, 1, 71},     {0x0246, 0x024E, 2, 1},
            {0x0370, 0x0372, 2, 1},      {0x0376, 0x0376, 1, 1},      {0x037F, 0x037F, 1, 116},
            {0x0386, 0x0386, 1, 38},     {0x0388, 0x038A, 1, 37},     {0x038C, 0x038C, 1, 64},
            {0x038E, 0x038F, 1, 63},     {0x0391, 0x03A1, 1, 32},     {0x03A3, 0x03AB, 1, 32},
            {0x03CF, 0x03CF, 1, 8},      {0x03D8, 0x03EE, 2, 1},      {0x03F4, 0x03F4, 1, -60},
            {0x03F7, 0x03F7, 1, 1},      {0x03F9, 0x03F9, 1, -7},     {0x03FA, 0x03FA, 1, 1},
            {0x03FD, 0x03FF, 1, -130},   {0x0400, 0x040F, 1, 80},     {0x0410, 0x042F, 1, 32},
            {0x0460, 0x0480, 2, 1},      {0x048A, 0x04BE, 2, 1},      {0x04C0, 0x04C0, 1, 15},
            {0x04C1, 0x04CD, 2, 1},      {0x04D0, 0x04FE, 2, 1},      {0x1E9E, 0x1E9E, 1, -7615},
            {0x2126, 0x2126, 1, -7517},  {0x212A, 0x212A, 1, -8383},  {0x212B, 0x212B, 1, -8262},
            {0x2C62, 0x2C62, 1, -10743}, {0x2C64, 0x2C64, 1, -10727}, {0x2C6D, 0x2C6D, 1, -10780},
            {0x2C6E, 0x2C6E, 1, -10749}, {0x2C6F, 0x2C6F, 1, -10783}, {0x2C70, 0x2C70, 1, -10782},
            {0x2C7E, 0x2C7F, 1, -10815}, {0xA78D, 0xA78D, 1, -42280}, {0xA7AA, 0xA7AA, 1, -42308},
            {0xA7AB, 0xA7AB, 1, -42319}, {0xA7AC, 0xA7AC, 1, -42315}, {0xA7AD, 0xA7AD, 1, -42305},
            {0xA7AE, 0xA7AE, 1, -42308}, {0xA7B0, 0xA7B0, 1, -42258}, {0xA7B1, 0xA7B1, 1, -42282},
            {0xA7B2, 0xA7B2, 1, -42261}, {0xA7C5, 0xA7C5, 1, -42307}, {0xA7CB, 0xA7CB, 1, -42343},
            {0xA7DC, 0xA7DC, 1, -42561},
        }};

        constexpr std::array<UpperExpansion, 5> UpperExpansions{{
            {0x00DF, {0x0053, 0x0053}, 2},
            {0x0149, {0x02BC, 0x004E}, 2},
            {0x01F0, {0x004A, 0x030C}, 2},
            {0x0390, {0x0399, 0x0308, 0x0301}, 3},
            {0x03B0, {0x03A5, 0x0308, 0x0301}, 3},
        }};

        constexpr std::array<CodepointRange, 36> CasedRanges{{
            {0x0041, 0x005A}, {0x0061, 0x007A}, {0x00B5, 0x00B5}, {0x00C0, 0x00D6},
            {0x00D8, 0x00F6}, {0x00F8, 0x01BA}, {0x01BC, 0x01BF}, {0x01C4, 0x0293},
            {0x0296, 0x02AF}, {0x0345, 0x0345}, {0x0370, 0x0373}, {0x0376, 0x0377},
            {0x037B, 0x037D}, {0x037F, 0x037F}, {0x0386, 0x0386}, {0x0388, 0x038A},
            {0x038C, 0x038C}, {0x038E, 0x03A1}, {0x03A3, 0x03F5}, {0x03F7, 0x0481},
            {0x048A, 0x04FF}, {0x1C80, 0x1C87}, {0x1E9E, 0x1E9E}, {0x1FBE, 0x1FBE},
            {0x2126, 0x2126}, {0x212A, 0x212B}, {0x2C62, 0x2C62}, {0x2C64, 0x2C66},
            {0x2C6D, 0x2C70}, {0x2C7E, 0x2C7F}, {0xA78D, 0xA78D}, {0xA7AA, 0xA7AE},
            {0xA7B0, 0xA7B2}, {0xA7C5, 0xA7C5}, {0xA7CB, 0xA7CB}, {0xA7DC, 0xA7DC},
        }};

        constexpr std::array<CodepointRange, 20> CaseIgnorableRanges{{
            {0x0027, 0x0027}, {0x002E, 0x002E}, {0x003A, 0x003A}, {0x005E, 0x005E},
            {0x0060, 0x0060}, {0x00A8, 0x00A8}, {0x00AD, 0x00AD}, {0x00AF, 0x00AF},
            {0x00B4, 0x00B4}, {0x00B7, 0x00B8}, {0x02B0, 0x036F}, {0x0374, 0x0375},
            {0x037A, 0x037A}, {0x0384, 0x0385}, {0x0387, 0x0387}, {0x0483, 0x0489},
            {0x200B, 0x200F}, {0x2018, 0x2019}, {0x2024, 0x2024}, {0x2027, 0x2027},
        }};
        // END GENERATED

        constexpr u32 CapitalSigma = 0x03A3;
        constexpr u32 SmallSigma = 0x03C3;
        constexpr u32 FinalSigma = 0x03C2;

        template <class T>
        constexpr bool IsAscending(const T& table)
        {
            for (usize i = 1; i < table.size(); ++i)
            {
                if (table[i].First <= table[i - 1].Last)
                {
                    return false;
                }
            }
            return true;
        }

        static_assert(IsAscending(UpperRuns) && IsAscending(LowerRuns));
        static_assert(IsAscending(CasedRanges) && IsAscending(CaseIgnorableRanges));

        /// @brief Applies the run covering @p c, or returns @p c when none does.
        u32 MapThrough(std::span<const CaseRun> runs, const u32 c)
        {
            const auto after = std::ranges::upper_bound(runs, c, {}, &CaseRun::First);
            if (after == runs.begin())
            {
                return c;
            }
            const CaseRun& run = *(after - 1);
            if (c > run.Last || (c - run.First) % run.Stride != 0)
            {
                return c;
            }
            return static_cast<u32>(static_cast<i32>(c) + run.Delta);
        }

        /// @brief Whether a range of @p ranges contains @p c.
        bool InRanges(std::span<const CodepointRange> ranges, const u32 c)
        {
            const auto after = std::ranges::upper_bound(ranges, c, {}, &CodepointRange::First);
            return after != ranges.begin() && c <= (after - 1)->Last;
        }

        /// @brief Decodes the well-formed UTF-8 sequence at byte @p i.
        ///
        /// @return The sequence's length in bytes, or 0 when the bytes at @p i are not a
        /// well-formed sequence (a stray continuation byte, a truncated, overlong or surrogate
        /// sequence, or one past U+10FFFF).
        usize Decode(const string_view text, const usize i, u32& codepoint)
        {
            const auto byte = [&](const usize k) { return static_cast<u8>(text[k]); };
            const u8 lead = byte(i);
            usize length = 0;
            u32 minimum = 0;
            if (lead < 0x80)
            {
                codepoint = lead;
                return 1;
            }
            if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                minimum = 0x80;
                codepoint = lead & 0x1Fu;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                minimum = 0x800;
                codepoint = lead & 0x0Fu;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                minimum = 0x10000;
                codepoint = lead & 0x07u;
            }
            else
            {
                return 0;
            }
            if (i + length > text.size())
            {
                return 0;
            }
            for (usize k = 1; k < length; ++k)
            {
                if ((byte(i + k) & 0xC0) != 0x80)
                {
                    return 0;
                }
                codepoint = (codepoint << 6) | (byte(i + k) & 0x3Fu);
            }
            const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
            if (codepoint < minimum || codepoint > 0x10FFFF || surrogate)
            {
                return 0;
            }
            return length;
        }

        /// @brief Appends a Unicode scalar value to @p out as UTF-8.
        void Encode(string& out, const u32 codepoint)
        {
            if (codepoint < 0x80)
            {
                out.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }

        /// @brief Whether a capital sigma ending just before byte @p next ends a word: no cased
        /// letter follows it, past any case-ignorable codepoints.
        bool EndsWord(const string_view text, usize next)
        {
            while (next < text.size())
            {
                u32 c = 0;
                const usize length = Decode(text, next, c);
                if (length == 0)
                {
                    return true;
                }
                if (!InRanges(CaseIgnorableRanges, c))
                {
                    return !InRanges(CasedRanges, c);
                }
                next += length;
            }
            return true;
        }
    }

    char32_t UpperCase(const char32_t c)
    {
        return static_cast<char32_t>(MapThrough(UpperRuns, static_cast<u32>(c)));
    }

    char32_t LowerCase(const char32_t c)
    {
        return static_cast<char32_t>(MapThrough(LowerRuns, static_cast<u32>(c)));
    }

    string UpperCase(const string_view text)
    {
        string out;
        out.reserve(text.size());
        for (usize i = 0; i < text.size();)
        {
            u32 c = 0;
            const usize length = Decode(text, i, c);
            if (length == 0)
            {
                out.push_back(text[i]);
                ++i;
                continue;
            }
            i += length;
            const auto expansion = std::ranges::find(UpperExpansions, c, &UpperExpansion::Source);
            if (expansion != UpperExpansions.end())
            {
                for (u32 k = 0; k < expansion->Length; ++k)
                {
                    Encode(out, expansion->Upper[k]);
                }
                continue;
            }
            Encode(out, MapThrough(UpperRuns, c));
        }
        return out;
    }

    string LowerCase(const string_view text)
    {
        string out;
        out.reserve(text.size());
        // Whether a cased letter precedes the current codepoint, past any case-ignorable ones:
        // the other half of the final-sigma condition.
        bool afterCased = false;
        for (usize i = 0; i < text.size();)
        {
            u32 c = 0;
            const usize length = Decode(text, i, c);
            if (length == 0)
            {
                out.push_back(text[i]);
                ++i;
                afterCased = false;
                continue;
            }
            i += length;
            if (c == CapitalSigma)
            {
                Encode(out, afterCased && EndsWord(text, i) ? FinalSigma : SmallSigma);
            }
            else
            {
                Encode(out, MapThrough(LowerRuns, c));
            }
            if (InRanges(CasedRanges, c))
            {
                afterCased = true;
            }
            else if (!InRanges(CaseIgnorableRanges, c))
            {
                afterCased = false;
            }
        }
        return out;
    }
}
