// LaunchArguments::Parse tests: pure, no GPU, no window. Proves the launcher
// argument grammar — the working-directory positional and the --level override.

#include <doctest/doctest.h>

#include <array>

#include <Veng/LaunchArguments.h>

using namespace Veng;

namespace
{
    // Parses a fixed token list, mirroring how Run hands Parse argv without argv[0].
    Result<LaunchArguments> ParseTokens(std::initializer_list<string> tokens)
    {
        const vector<string> args(tokens);
        return LaunchArguments::Parse(args);
    }
}

TEST_CASE("LaunchArguments: no arguments yields all-unset")
{
    const Result<LaunchArguments> parsed = LaunchArguments::Parse({});
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->WorkingDirectory.has_value());
    CHECK_FALSE(parsed->Level.has_value());
}

TEST_CASE("LaunchArguments: a leading positional is the working directory")
{
    const Result<LaunchArguments> parsed = ParseTokens({"/some/dir"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->WorkingDirectory.has_value());
    CHECK(*parsed->WorkingDirectory == path("/some/dir"));
    CHECK_FALSE(parsed->Level.has_value());
}

TEST_CASE("LaunchArguments: --level=<decimal> sets the level override")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--level=12345"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Level.has_value());
    CHECK(parsed->Level->Value == 12345ULL);
}

TEST_CASE("LaunchArguments: --level=<hex> accepts a 0x-prefixed id")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--level=0xABCDEF0123456789"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Level.has_value());
    CHECK(parsed->Level->Value == 0xABCDEF0123456789ULL);
}

TEST_CASE("LaunchArguments: --level <id> accepts a separated value")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--level", "0x10"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Level.has_value());
    CHECK(parsed->Level->Value == 0x10ULL);
}

TEST_CASE("LaunchArguments: the working directory and --level combine")
{
    const Result<LaunchArguments> parsed = ParseTokens({"/work", "--level=42"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->WorkingDirectory.has_value());
    CHECK(*parsed->WorkingDirectory == path("/work"));
    REQUIRE(parsed->Level.has_value());
    CHECK(parsed->Level->Value == 42ULL);
}

TEST_CASE("LaunchArguments: --level=0 is rejected as the reserved invalid id")
{
    CHECK_FALSE(ParseTokens({"--level=0"}).has_value());
}

TEST_CASE("LaunchArguments: a malformed level id is rejected")
{
    CHECK_FALSE(ParseTokens({"--level=notanumber"}).has_value());
    CHECK_FALSE(ParseTokens({"--level=12x"}).has_value());
    CHECK_FALSE(ParseTokens({"--level="}).has_value());
}

TEST_CASE("LaunchArguments: --level with no value is rejected")
{
    CHECK_FALSE(ParseTokens({"--level"}).has_value());
}

TEST_CASE("LaunchArguments: an unknown flag is rejected")
{
    CHECK_FALSE(ParseTokens({"--nope"}).has_value());
}

TEST_CASE("LaunchArguments: a second positional argument is rejected")
{
    CHECK_FALSE(ParseTokens({"/work", "/extra"}).has_value());
}
