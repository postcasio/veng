// The shared case mapper: every cased letter of each covered block round-trips through its simple
// mappings, apart from the letters Unicode maps one way only; the string forms expand `ß` and
// write a word-final sigma; and anything uncased or ill-formed passes through. Aggregate
// assertions over whole blocks — each walk is a few hundred codepoints.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>

#include <Veng/Text/CaseMapping.h>

using namespace Veng;
using namespace Veng::Text;

namespace
{
    // Letters whose mapping does not invert: their partner maps back to a different letter (the
    // dotless i, the long s, a titlecase digraph, the Greek symbol variants and final sigma).
    constexpr std::array<u32, 15> OneWaySmalls{0x00B5, 0x0131, 0x017F, 0x01C5, 0x01C8,
                                               0x01CB, 0x01F2, 0x03C2, 0x03D0, 0x03D1,
                                               0x03D5, 0x03D6, 0x03F0, 0x03F1, 0x03F5};
    constexpr std::array<u32, 6> OneWayCapitals{0x0130, 0x01C5, 0x01C8, 0x01CB, 0x01F2, 0x03F4};

    struct BlockWalk
    {
        // Small letters whose capital lowers back to them.
        u32 RoundTrips = 0;
        // Letters, of either case, that failed to invert and are not one-way by Unicode.
        u32 Broken = 0;
    };

    BlockWalk Walk(const u32 first, const u32 last)
    {
        BlockWalk walk;
        for (u32 c = first; c <= last; ++c)
        {
            const auto cp = static_cast<char32_t>(c);
            const char32_t upper = UpperCase(cp);
            const char32_t lower = LowerCase(cp);
            if (upper != cp)
            {
                if (LowerCase(upper) == cp)
                {
                    ++walk.RoundTrips;
                }
                else if (std::ranges::find(OneWaySmalls, c) == OneWaySmalls.end())
                {
                    ++walk.Broken;
                }
            }
            if (lower != cp && UpperCase(lower) != cp &&
                std::ranges::find(OneWayCapitals, c) == OneWayCapitals.end())
            {
                ++walk.Broken;
            }
        }
        return walk;
    }
}

TEST_CASE("case mapping: every cased letter of each covered block round-trips")
{
    struct Block
    {
        const char* Name;
        u32 First;
        u32 Last;
        u32 Pairs;
    };
    // The floors are coverage claims: ASCII and Latin-1 pair every letter (26 + 30), and each
    // further block is mapped substantially rather than left to pass through.
    constexpr std::array<Block, 5> Blocks{{
        {.Name = "ASCII and Latin-1", .First = 0x0000, .Last = 0x00FF, .Pairs = 56},
        {.Name = "Latin Extended-A", .First = 0x0100, .Last = 0x017F, .Pairs = 60},
        {.Name = "Latin Extended-B", .First = 0x0180, .Last = 0x024F, .Pairs = 80},
        {.Name = "Greek and Coptic", .First = 0x0370, .Last = 0x03FF, .Pairs = 50},
        {.Name = "Cyrillic", .First = 0x0400, .Last = 0x04FF, .Pairs = 120},
    }};
    for (const Block& block : Blocks)
    {
        CAPTURE(block.Name);
        const BlockWalk walk = Walk(block.First, block.Last);
        CHECK(walk.Broken == 0);
        CHECK(walk.RoundTrips >= block.Pairs);
    }
}

TEST_CASE("case mapping: letters across the covered scripts map to their partners")
{
    CHECK(UpperCase(U'à') == U'À');
    CHECK(UpperCase(U'ÿ') == U'Ÿ');
    CHECK(LowerCase(U'Ÿ') == U'ÿ');
    CHECK(UpperCase(U'ő') == U'Ő');
    CHECK(UpperCase(U'ƀ') == U'Ƀ');
    CHECK(UpperCase(U'ǆ') == U'Ǆ');
    CHECK(UpperCase(U'ά') == U'Ά');
    CHECK(UpperCase(U'ω') == U'Ω');
    CHECK(UpperCase(U'ж') == U'Ж');
    CHECK(UpperCase(U'ё') == U'Ё');
    CHECK(LowerCase(U'Ґ') == U'ґ');
    // The capital dotted I lowers to a plain i, not to the dotless i beside it.
    CHECK(LowerCase(U'İ') == U'i');
}

TEST_CASE("case mapping: the string forms apply the expansions and the final sigma")
{
    CHECK(UpperCase("straße") == "STRASSE");
    CHECK(UpperCase("ΐ") == "Ϊ́");

    // A capital sigma closing a word lowers to the final form, and only there.
    CHECK(LowerCase("ΟΔΟΣ") == "οδος");
    CHECK(LowerCase("ΟΔΟΣ").ends_with("ς"));
    CHECK(LowerCase("ΣΟΦΟΣ ΣΟΦΟΣ.") == "σοφος σοφος.");
    CHECK(LowerCase("Σ") == "σ");
    CHECK(LowerCase("ΑΣ́Β") == "ασ́β");

    CHECK(UpperCase("Café Ωmega привет") == "CAFÉ ΩMEGA ПРИВЕТ");
    CHECK(LowerCase("CAFÉ ΩMEGA ПРИВЕТ") == "café ωmega привет");
}

TEST_CASE("case mapping: uncased and ill-formed text passes through unchanged")
{
    CHECK(UpperCase("東京 2049 — ok!") == "東京 2049 — OK!");
    CHECK(LowerCase("東京 2049 — OK!") == "東京 2049 — ok!");
    CHECK(UpperCase(U'7') == U'7');
    CHECK(LowerCase(U'東') == U'東');

    // A truncated sequence, a stray continuation byte, an overlong encoding and an encoded
    // surrogate each survive byte for byte, while the letters around them still map.
    CHECK(UpperCase("a\xC3") == "A\xC3");
    CHECK(UpperCase("\x80"
                    "b") == "\x80"
                            "B");
    CHECK(LowerCase("\xC0\xAF"
                    "Z") == "\xC0\xAF"
                            "z");
    CHECK(UpperCase("\xED\xA0\x80"
                    "é") == "\xED\xA0\x80"
                            "É");
}
