// The device-free message-format library: named-field substitution and reorder, locale number
// separators, per-locale CLDR plural selection, within-message fallback to the Other variant, and
// the load-bearing property that a malformed template returns an error rather than letting fmt
// throw across veng's -fno-exceptions boundary. Property assertions over stratified inputs — no
// swept population, no per-iteration asserts.

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
