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

    // Parses a fixed token list against an application's declared options, mirroring how Run hands
    // Parse the app's ApplicationInfo::LaunchOptions.
    Result<LaunchArguments> ParseTokens(std::initializer_list<string> tokens,
                                        std::initializer_list<LaunchOptionInfo> options)
    {
        const vector<string> args(tokens);
        const vector<LaunchOptionInfo> declared(options);
        return LaunchArguments::Parse(args, declared);
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

TEST_CASE("LaunchArguments: --server sets the server flag")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--server"});
    REQUIRE(parsed.has_value());
    CHECK(parsed->Server);
    CHECK_FALSE(parsed->Headless);
    CHECK_FALSE(parsed->Join.has_value());
}

TEST_CASE("LaunchArguments: --server --headless is the dedicated-server pair")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--server", "--headless"});
    REQUIRE(parsed.has_value());
    CHECK(parsed->Server);
    CHECK(parsed->Headless);
}

TEST_CASE("LaunchArguments: --background-input is an independent flag, off by default")
{
    const Result<LaunchArguments> bare = ParseTokens({});
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->BackgroundInput);

    // It is orthogonal to the windowing flags: a windowed run is exactly what it is for.
    const Result<LaunchArguments> parsed = ParseTokens({"--background-input", "--level=42"});
    REQUIRE(parsed.has_value());
    CHECK(parsed->BackgroundInput);
    CHECK_FALSE(parsed->Headless);
    CHECK(parsed->Level.has_value());
}

TEST_CASE("LaunchArguments: --no-render is an independent flag, off by default")
{
    const Result<LaunchArguments> bare = ParseTokens({"--headless"});
    REQUIRE(bare.has_value());
    // A headless run renders unless it says otherwise: the flag is the opt-out, not the default.
    CHECK(bare->Headless);
    CHECK_FALSE(bare->NoRender);

    const Result<LaunchArguments> parsed =
        ParseTokens({"--headless", "--no-render", "--join", "h"});
    REQUIRE(parsed.has_value());
    CHECK(parsed->Headless);
    CHECK(parsed->NoRender);
    CHECK(parsed->Join.has_value());

    // --dedicated already implies no render tail, so it leaves the explicit flag clear.
    const Result<LaunchArguments> dedicated = ParseTokens({"--dedicated"});
    REQUIRE(dedicated.has_value());
    CHECK_FALSE(dedicated->NoRender);
}

TEST_CASE("LaunchArguments: --dedicated is a first-class synonym for --server --headless")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--dedicated"});
    REQUIRE(parsed.has_value());
    // The dedicated flag sets both arms, so the process drives the identical ServerHost path headless.
    CHECK(parsed->Dedicated);
    CHECK(parsed->Server);
    CHECK(parsed->Headless);
    CHECK_FALSE(parsed->Join.has_value());
}

TEST_CASE("LaunchArguments: --join host:port splits into host and port")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--join", "127.0.0.1:27750"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Join.has_value());
    CHECK(parsed->Join->Host == "127.0.0.1");
    CHECK(parsed->Join->Port == 27750);
}

TEST_CASE("LaunchArguments: --join=host uses the default port (0)")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--join=example.lan"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Join.has_value());
    CHECK(parsed->Join->Host == "example.lan");
    CHECK(parsed->Join->Port == 0);
}

TEST_CASE("LaunchArguments: --join with no value is rejected")
{
    CHECK_FALSE(ParseTokens({"--join"}).has_value());
}

TEST_CASE("LaunchArguments: a malformed --join port is rejected")
{
    CHECK_FALSE(ParseTokens({"--join", "host:notaport"}).has_value());
    CHECK_FALSE(ParseTokens({"--join", ":27750"}).has_value());
}

TEST_CASE("LaunchArguments: --name sets the local player name")
{
    const Result<LaunchArguments> separated = ParseTokens({"--name", "ada"});
    REQUIRE(separated.has_value());
    REQUIRE(separated->Name.has_value());
    CHECK(*separated->Name == "ada");

    const Result<LaunchArguments> joined = ParseTokens({"--name=grace"});
    REQUIRE(joined.has_value());
    REQUIRE(joined->Name.has_value());
    CHECK(*joined->Name == "grace");
}

TEST_CASE("LaunchArguments: --name with no or an empty value is rejected")
{
    CHECK_FALSE(ParseTokens({"--name"}).has_value());
    CHECK_FALSE(ParseTokens({"--name="}).has_value());
}

TEST_CASE("LaunchArguments: an unknown flag is rejected")
{
    CHECK_FALSE(ParseTokens({"--nope"}).has_value());
}

TEST_CASE("LaunchArguments: a second positional argument is rejected")
{
    CHECK_FALSE(ParseTokens({"/work", "/extra"}).has_value());
}

TEST_CASE("LaunchArguments: --netsim parses latency/jitter (ms) and loss/dup/reorder (percent)")
{
    const Result<LaunchArguments> parsed =
        ParseTokens({"--netsim", "latency=100,jitter=20,loss=5,dup=1,reorder=2,seed=7"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->NetSim.has_value());
    CHECK(parsed->NetSim->LatencyMs == doctest::Approx(100.0f));
    CHECK(parsed->NetSim->JitterMs == doctest::Approx(20.0f));
    CHECK(parsed->NetSim->DropRate == doctest::Approx(0.05f));
    CHECK(parsed->NetSim->DuplicateRate == doctest::Approx(0.01f));
    CHECK(parsed->NetSim->ReorderRate == doctest::Approx(0.02f));
    CHECK(parsed->NetSim->Seed == 7u);
}

TEST_CASE("LaunchArguments: an unknown --netsim key is rejected")
{
    CHECK_FALSE(ParseTokens({"--netsim", "bogus=1"}).has_value());
    CHECK_FALSE(ParseTokens({"--netsim", "latency"}).has_value());
}

TEST_CASE("LaunchArguments: no --netsim leaves the link clean")
{
    const Result<LaunchArguments> parsed = ParseTokens({"--server"});
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->NetSim.has_value());
}

TEST_CASE("LaunchArguments: a declared value-taking option consumes its value")
{
    const Result<LaunchArguments> separated =
        ParseTokens({"--foo", "bar"}, {{.Name = "foo", .TakesValue = true}});
    REQUIRE(separated.has_value());
    REQUIRE(separated->GameOptions.contains("foo"));
    CHECK(separated->GameOptions.at("foo") == "bar");

    const Result<LaunchArguments> joined =
        ParseTokens({"--foo=bar"}, {{.Name = "foo", .TakesValue = true}});
    REQUIRE(joined.has_value());
    REQUIRE(joined->GameOptions.contains("foo"));
    CHECK(joined->GameOptions.at("foo") == "bar");
}

TEST_CASE("LaunchArguments: a declared value-less option maps to an empty string")
{
    const Result<LaunchArguments> parsed =
        ParseTokens({"--verbose"}, {{.Name = "verbose", .TakesValue = false}});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->GameOptions.contains("verbose"));
    CHECK(parsed->GameOptions.at("verbose").empty());
}

TEST_CASE("LaunchArguments: a declared option absent from the command line has no entry")
{
    const Result<LaunchArguments> parsed =
        ParseTokens({"--server"}, {{.Name = "foo", .TakesValue = true}});
    REQUIRE(parsed.has_value());
    CHECK(parsed->GameOptions.empty());
}

TEST_CASE("LaunchArguments: an undeclared flag is still rejected alongside a declared one")
{
    CHECK_FALSE(ParseTokens({"--nope"}, {{.Name = "foo", .TakesValue = true}}).has_value());
    CHECK_FALSE(
        ParseTokens({"--foo", "bar", "--nope"}, {{.Name = "foo", .TakesValue = true}}).has_value());
}

TEST_CASE("LaunchArguments: a declared option missing its value is rejected")
{
    CHECK_FALSE(ParseTokens({"--foo"}, {{.Name = "foo", .TakesValue = true}}).has_value());
    CHECK_FALSE(
        ParseTokens({"--verbose=1"}, {{.Name = "verbose", .TakesValue = false}}).has_value());
}

TEST_CASE("LaunchArguments: an engine flag of the same name wins over a declared option")
{
    const Result<LaunchArguments> parsed =
        ParseTokens({"--level=42"}, {{.Name = "level", .TakesValue = true}});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->Level.has_value());
    CHECK(parsed->Level->Value == 42ULL);
    CHECK(parsed->GameOptions.empty());
}

TEST_CASE("LaunchArguments: declaring no options leaves the engine grammar unchanged")
{
    const std::initializer_list<string> tokens = {"/work",  "--level=42", "--server",
                                                  "--name", "ada",        "--join=example.lan"};
    const Result<LaunchArguments> bare = ParseTokens(tokens);
    const Result<LaunchArguments> declared = ParseTokens(tokens, {});
    REQUIRE(bare.has_value());
    REQUIRE(declared.has_value());
    CHECK(*bare->WorkingDirectory == *declared->WorkingDirectory);
    CHECK(bare->Level->Value == declared->Level->Value);
    CHECK(bare->Server == declared->Server);
    CHECK(*bare->Name == *declared->Name);
    CHECK(bare->Join->Host == declared->Join->Host);
    CHECK(bare->GameOptions.empty());
    CHECK(declared->GameOptions.empty());

    // With nothing declared, every `--` token is still unknown.
    CHECK_FALSE(ParseTokens({"--foo", "bar"}, {}).has_value());
}
