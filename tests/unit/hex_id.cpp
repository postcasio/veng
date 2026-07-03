// Hex-id codec tests: the canonical zero-padded "0x" + 16-uppercase-hex string
// form and its round-trip/rejection contract. Pure, no GPU.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/HexId.h>

using namespace Veng;

TEST_CASE("hex_id round-trips a spread including the boundary")
{
    const u64 values[] = {
        1,
        0x3E9,
        0x0D49F2A1C03B5E76ULL,
        0xFFFFFFFFFFFFFFFFULL,
    };

    for (const u64 v : values)
    {
        const optional<u64> parsed = ParseHexId(FormatHexId(v));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == v);
    }
}

TEST_CASE("hex_id formats zero-padded to 16 uppercase digits")
{
    CHECK(FormatHexId(0x3E9) == "0x00000000000003E9");
    CHECK(FormatHexId(0) == "0x0000000000000000");
    CHECK(FormatHexId(0xFFFFFFFFFFFFFFFFULL) == "0xFFFFFFFFFFFFFFFF");
}

TEST_CASE("hex_id parse tolerates padding, case, and a missing prefix")
{
    CHECK(ParseHexId("0x00000000000003E9") == optional<u64>(0x3E9));
    CHECK(ParseHexId("0x3e9") == optional<u64>(0x3E9));
    CHECK(ParseHexId("3E9") == optional<u64>(0x3E9));
    CHECK(ParseHexId("0X3E9") == optional<u64>(0x3E9));
}

TEST_CASE("hex_id parse rejects empty, prefix-only, non-hex, trailing junk, and overflow")
{
    CHECK_FALSE(ParseHexId("").has_value());
    CHECK_FALSE(ParseHexId("0x").has_value());
    CHECK_FALSE(ParseHexId("0xG").has_value());
    CHECK_FALSE(ParseHexId("0x12cat").has_value());
    CHECK_FALSE(ParseHexId("0x1FFFFFFFFFFFFFFFF").has_value());
    CHECK_FALSE(ParseHexId("12345678901234567890").has_value());
}

TEST_CASE("hex_id parses 0x0 to zero — the caller owns the invalid-sentinel split")
{
    const optional<u64> parsed = ParseHexId("0x0");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == 0);
}

TEST_CASE("hex_id AssetId wrappers delegate to the u64 codec")
{
    const AssetId id{0x0D49F2A1C03B5E76ULL};
    CHECK(FormatAssetId(id) == "0x0D49F2A1C03B5E76");

    const optional<AssetId> parsed = ParseAssetId(FormatAssetId(id));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == id);
}
