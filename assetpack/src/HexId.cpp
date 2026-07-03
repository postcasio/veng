#include <Veng/Asset/HexId.h>

#include <charconv>

#include <fmt/format.h>

namespace Veng
{
    string FormatHexId(u64 value)
    {
        return fmt::format("0x{:016X}", value);
    }

    optional<u64> ParseHexId(std::string_view text)
    {
        std::string_view digits = text;

        // Skip an optional 0x/0X prefix; from_chars parses the bare hex digits only.
        if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        {
            digits.remove_prefix(2);
        }

        if (digits.empty())
        {
            return std::nullopt;
        }

        const char* const first = digits.data();
        const char* const last = digits.data() + digits.size();

        u64 value = 0;
        const std::from_chars_result result = std::from_chars(first, last, value, 16);

        // Reject overflow (result_out_of_range), a non-hex leading character, and any
        // trailing garbage past the parsed run.
        if (result.ec != std::errc{} || result.ptr != last)
        {
            return std::nullopt;
        }

        return value;
    }

    string FormatAssetId(AssetId id)
    {
        return FormatHexId(id.Value);
    }

    optional<AssetId> ParseAssetId(std::string_view text)
    {
        return ParseHexId(text).transform([](u64 value) { return AssetId{value}; });
    }
}
