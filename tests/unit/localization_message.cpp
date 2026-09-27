// The device-free message-format library: named-field substitution and reorder, locale number
// separators, per-locale CLDR plural selection, within-message fallback to the Other variant,
// elision of a template's word before a vowel-initial value, and the load-bearing property that a
// malformed template returns an error rather than letting fmt throw across veng's -fno-exceptions
// boundary. Property assertions over stratified inputs — no swept population, no per-iteration
// asserts.

#include <doctest/doctest.h>

#include <array>
#include <span>

#include <Veng/Localization/Message.h>

using namespace Veng;
using namespace Veng::Localization;

namespace
{
    // Formats with English separators unless a case overrides them, keeping each call to the parts
    // it actually exercises.
    Result<string> Format(const Message& message, std::span<const FormatArg> args,
                          optional<i64> count = std::nullopt, string_view locale = "en",
                          NumberFormat numbers = {})
    {
        return FormatMessage(message, args, count, PluralRuleFor(locale), numbers);
    }

    Message Template(string text)
    {
        return Message{.Template = std::move(text)};
    }
}

TEST_CASE("named fields bind by name and reorder freely")
{
    const std::array<FormatArg, 2> args{
        FormatArg{.Name = "name", .Value = string_view("Vega")},
        FormatArg{.Name = "dist", .Value = 4.2},
    };

    const Result<string> forward = Format(Template("{name} is {dist:.1f} ly away"), args);
    REQUIRE(forward.has_value());
    CHECK(*forward == "Vega is 4.2 ly away");

    // The same argument set, a reordered template — the word-order property a translator relies on.
    const Result<string> reordered = Format(Template("{dist:.1f} ly to {name}"), args);
    REQUIRE(reordered.has_value());
    CHECK(*reordered == "4.2 ly to Vega");
}

TEST_CASE("literal braces are written by doubling")
{
    const Result<string> r = Format(Template("{{not a field}} but {x} is"),
                                    std::array<FormatArg, 1>{FormatArg{.Name = "x", .Value = 7}});
    REQUIRE(r.has_value());
    CHECK(*r == "{not a field} but 7 is");
}

TEST_CASE("numeric arguments take the locale's separators")
{
    const std::array<FormatArg, 1> big{FormatArg{.Name = "n", .Value = static_cast<i64>(12000)}};
    const std::array<FormatArg, 1> frac{FormatArg{.Name = "d", .Value = 1.5}};

    const NumberFormat french{.Decimal = U',', .Grouping = U' '};

    const Result<string> grouped = Format(Template("{n}"), big, std::nullopt, "fr", french);
    REQUIRE(grouped.has_value());
    CHECK(*grouped == "12 000");

    const Result<string> decimal = Format(Template("{d:.1f}"), frac, std::nullopt, "fr", french);
    REQUIRE(decimal.has_value());
    CHECK(*decimal == "1,5");

    // The default NumberFormat is English: a decimal point and a comma thousands separator.
    const Result<string> englishGrouped = Format(Template("{n}"), big);
    REQUIRE(englishGrouped.has_value());
    CHECK(*englishGrouped == "12,000");

    const Result<string> englishDecimal = Format(Template("{d:.1f}"), frac);
    REQUIRE(englishDecimal.has_value());
    CHECK(*englishDecimal == "1.5");

    // A grouping separator of U'\0' turns grouping off.
    const Result<string> ungrouped =
        Format(Template("{n}"), big, std::nullopt, "en", NumberFormat{.Grouping = U'\0'});
    REQUIRE(ungrouped.has_value());
    CHECK(*ungrouped == "12000");
}

TEST_CASE("plural selection picks the count's variant and {#} prints the count")
{
    std::array<string, PluralCategoryCount> jumps{};
    jumps[static_cast<usize>(PluralCategory::One)] = "{#} jump left";
    jumps[static_cast<usize>(PluralCategory::Other)] = "{#} jumps left";
    const Message message{.Plurals = jumps};

    SUBCASE("English distinguishes only the singular")
    {
        const Result<string> one = Format(message, {}, 1, "en");
        REQUIRE(one.has_value());
        CHECK(*one == "1 jump left");

        const Result<string> many = Format(message, {}, 3, "en");
        REQUIRE(many.has_value());
        CHECK(*many == "3 jumps left");
    }

    SUBCASE("French groups zero with one")
    {
        const Result<string> zero = Format(message, {}, 0, "fr");
        REQUIRE(zero.has_value());
        CHECK(*zero == "0 jump left");
    }
}

TEST_CASE("the Polish three-form rule lands counts in the right category")
{
    const PluralRule polish = PluralRuleFor("pl");
    CHECK(polish(1) == PluralCategory::One);
    CHECK(polish(2) == PluralCategory::Few);
    CHECK(polish(5) == PluralCategory::Many);
    CHECK(polish(22) == PluralCategory::Few);
    CHECK(polish(12) == PluralCategory::Many);
    CHECK(polish(0) == PluralCategory::Many);

    std::array<string, PluralCategoryCount> forms{};
    forms[static_cast<usize>(PluralCategory::One)] = "one";
    forms[static_cast<usize>(PluralCategory::Few)] = "few";
    forms[static_cast<usize>(PluralCategory::Many)] = "many";
    forms[static_cast<usize>(PluralCategory::Other)] = "other";
    const Message message{.Plurals = forms};

    CHECK(*Format(message, {}, 1, "pl") == "one");
    CHECK(*Format(message, {}, 22, "pl") == "few");
    CHECK(*Format(message, {}, 12, "pl") == "many");
}

TEST_CASE("plural rules resolve by primary subtag with an English default")
{
    // The primary subtag alone is significant, and an unknown id falls back to the English rule.
    CHECK(PluralRuleFor("en-US")(1) == PluralCategory::One);
    CHECK(PluralRuleFor("en-US")(2) == PluralCategory::Other);
    CHECK(PluralRuleFor("de")(1) == PluralCategory::One);
    CHECK(PluralRuleFor("tr")(5) == PluralCategory::Other);
    CHECK(PluralRuleFor("cs")(3) == PluralCategory::Few);
    CHECK(PluralRuleFor("xx")(1) == PluralCategory::One);
    CHECK(PluralRuleFor("xx")(2) == PluralCategory::Other);
}

TEST_CASE("a missing plural variant falls back to Other")
{
    // The message defines only Other; English selects One for a count of 1, which is absent.
    std::array<string, PluralCategoryCount> variants{};
    variants[static_cast<usize>(PluralCategory::Other)] = "{#} items";
    const Message message{.Plurals = variants};

    const Result<string> r = Format(message, {}, 1, "en");
    REQUIRE(r.has_value());
    CHECK(*r == "1 items");
}

TEST_CASE("{#} outside a plural template still binds the count")
{
    const Result<string> r = Format(Template("{#} ready"), {}, 3);
    REQUIRE(r.has_value());
    CHECK(*r == "3 ready");
}

TEST_CASE("a malformed template returns an error and never lets fmt throw")
{
    // Message.cpp compiles -fno-exceptions, so a template that reached fmt's error path would
    // terminate the process rather than throw. Each of these must come back as an Err instead.

    SUBCASE("an unbound field")
    {
        const Result<string> r = Format(Template("{missing}"), {});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("an unbalanced brace")
    {
        const Result<string> r = Format(Template("{oops"), {});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("an unmatched closing brace")
    {
        const Result<string> r = Format(Template("oops}"), {});
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("a float spec on a string argument")
    {
        const std::array<FormatArg, 1> args{FormatArg{.Name = "s", .Value = string_view("hi")}};
        const Result<string> r = Format(Template("{s:.1f}"), args);
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("a precision on an integer argument")
    {
        const std::array<FormatArg, 1> args{FormatArg{.Name = "n", .Value = static_cast<i64>(4)}};
        const Result<string> r = Format(Template("{n:.2f}"), args);
        CHECK_FALSE(r.has_value());
    }

    SUBCASE("{#} without a count")
    {
        const Result<string> r = Format(Template("{#} left"), {});
        CHECK_FALSE(r.has_value());
    }
}

TEST_CASE("fmt specs and non-numeric arguments format through")
{
    const std::array<FormatArg, 3> args{
        FormatArg{.Name = "flag", .Value = true},
        FormatArg{.Name = "hex", .Value = static_cast<i64>(255)},
        FormatArg{.Name = "pad", .Value = string_view("hi")},
    };
    const Result<string> r = Format(Template("{flag} {hex:x} {pad:>4}"), args);
    REQUIRE(r.has_value());
    CHECK(*r == "true ff   hi");
}

namespace
{
    // A French elision table as a catalog would author it: the vowels and their accented forms,
    // with h left out, and the words that elide before them.
    ElisionTable FrenchElision()
    {
        return ElisionTable{
            .Initials = "aeiouàâäéèêëîïôöùûüæœ",
            .Rules = {{.Word = "de", .Elided = "d'"},
                      {.Word = "du", .Elided = "de l'"},
                      {.Word = "le", .Elided = "l'"},
                      {.Word = "la", .Elided = "l'"},
                      {.Word = "que", .Elided = "qu'"}},
        };
    }

    // Formats a template against one string field under the French table.
    string Elided(string text, string_view value, string_view field = "name")
    {
        const std::array<FormatArg, 1> args{FormatArg{.Name = field, .Value = value}};
        const Result<string> r = FormatMessage(Template(std::move(text)), args, std::nullopt,
                                               PluralRuleFor("fr"), {}, FrenchElision());
        REQUIRE(r.has_value());
        return *r;
    }
}

TEST_CASE("a listed word elides before a vowel and stays before a consonant")
{
    CHECK(Elided("le livre de {name}", "Anne") == "le livre d'Anne");
    CHECK(Elided("le livre de {name}", "Paul") == "le livre de Paul");
    CHECK(Elided("le prix du {name}", "orange") == "le prix de l'orange");
    CHECK(Elided("le prix du {name}", "pain") == "le prix du pain");
    CHECK(Elided("voici le {name}", "arbre") == "voici l'arbre");
    CHECK(Elided("je crois que {name} viendra", "Isabelle") == "je crois qu'Isabelle viendra");
    // h is not an eliding initial in this table, so the word stays as written before it.
    CHECK(Elided("le livre de {name}", "Hugo") == "le livre de Hugo");
}

TEST_CASE("an all-capital initialism does not elide; a capitalized word or a lone capital does")
{
    CHECK(Elided("la carte de {name}", "ABC 1234") == "la carte de ABC 1234");
    CHECK(Elided("la carte de {name}", "EU-7") == "la carte de EU-7");
    CHECK(Elided("la carte de {name}", "Ontario") == "la carte d'Ontario");
    CHECK(Elided("la carte de {name}", "A 12") == "la carte d'A 12");
    CHECK(Elided("la carte de {name}", "Ohio Nord") == "la carte d'Ohio Nord");
}

TEST_CASE("accented initials elide in either case")
{
    CHECK(Elided("le livre de {name}", "Émile") == "le livre d'Émile");
    CHECK(Elided("le livre de {name}", "élan") == "le livre d'élan");
    CHECK(Elided("le livre de {name}", "Île verte") == "le livre d'Île verte");
    CHECK(Elided("le livre de {name}", "Ôde") == "le livre d'Ôde");
    CHECK(Elided("le livre de {name}", "Œuvre") == "le livre d'Œuvre");
    CHECK(Elided("le livre de {name}", "Ça") == "le livre de Ça");
}

TEST_CASE("only a whole listed word of the template, one space before the field, elides")
{
    // No word before the field, or a word the table does not list.
    CHECK(Elided("{name} de", "Anne") == "Anne de");
    CHECK(Elided("{name}", "Anne") == "Anne");
    CHECK(Elided("grande {name}", "Anne") == "grande Anne");
    // A listed word must stand alone: "made" ends in "de" but is not "de".
    CHECK(Elided("made {name}", "Anne") == "made Anne");
    // Any separator but one ASCII space opts the field out.
    CHECK(Elided("de {name}", "Anne") == "de Anne");
    CHECK(Elided("de  {name}", "Anne") == "de  Anne");
    CHECK(Elided("de{name}", "Anne") == "deAnne");

    // Text an earlier field substituted never elides; only the template's own words do.
    const std::array<FormatArg, 2> args{FormatArg{.Name = "a", .Value = string_view("livre de")},
                                        FormatArg{.Name = "b", .Value = string_view("Anne")}};
    const Result<string> substituted = FormatMessage(Template("{a} {b}"), args, std::nullopt,
                                                     PluralRuleFor("fr"), {}, FrenchElision());
    REQUIRE(substituted.has_value());
    CHECK(*substituted == "livre de Anne");
}

TEST_CASE("the elided form takes the case the template wrote the word in")
{
    CHECK(Elided("De {name}", "Anne") == "D'Anne");
    CHECK(Elided("DE {name}", "Anne") == "D'Anne");
    CHECK(Elided("Du {name}", "orange") == "De l'orange");
    CHECK(Elided("LE PRIX DU {name}", "Orange") == "LE PRIX DE L'Orange");
}

TEST_CASE("elision runs through every template path and an empty table elides nothing")
{
    // With no table the word is left as written.
    const std::array<FormatArg, 1> anne{FormatArg{.Name = "name", .Value = string_view("Anne")}};
    const Result<string> plain = Format(Template("le livre de {name}"), anne);
    REQUIRE(plain.has_value());
    CHECK(*plain == "le livre de Anne");

    // Each plural variant elides, beside a numeric field and the {#} count.
    const Message plural{.Plurals = std::array<string, PluralCategoryCount>{
                             "", "{#} livre de {name}", "", "", "", "{#} livres de {name}"}};
    const Result<string> one =
        FormatMessage(plural, anne, 1, PluralRuleFor("fr"), {}, FrenchElision());
    const Result<string> many =
        FormatMessage(plural, anne, 3, PluralRuleFor("fr"), {}, FrenchElision());
    REQUIRE(one.has_value());
    REQUIRE(many.has_value());
    CHECK(*one == "1 livre d'Anne");
    CHECK(*many == "3 livres d'Anne");

    // A value rendered with padding or as a number begins with no letter, so nothing elides.
    CHECK(Elided("le livre de {name:>6}", "Anne") == "le livre de   Anne");
    const std::array<FormatArg, 1> number{FormatArg{.Name = "n", .Value = static_cast<i64>(8)}};
    const Result<string> numeric = FormatMessage(Template("le numéro de {n}"), number, std::nullopt,
                                                 PluralRuleFor("fr"), {}, FrenchElision());
    REQUIRE(numeric.has_value());
    CHECK(*numeric == "le numéro de 8");
}
