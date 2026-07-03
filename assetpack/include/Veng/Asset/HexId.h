#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Types.h>

#include <string_view>

namespace Veng
{
    /// @brief Formats a 64-bit id as the canonical "0x" + 16-uppercase-hex-digit string.
    /// @param value  The id value to format.
    /// @return The zero-padded canonical string, e.g. "0x0D49F2A1C03B5E76".
    string FormatHexId(u64 value);

    /// @brief Parses a hex id string (optional 0x/0X prefix, case-insensitive) to a u64.
    ///
    /// Returns nullopt on empty input, overflow past 64 bits, or any non-hex or trailing
    /// character — the reader turns nullopt into a located error. Value 0 ("0x0") parses
    /// successfully; whether a zero id is the reserved invalid sentinel is the caller's
    /// decision, not the codec's.
    /// @param text  The candidate hex-id string.
    /// @return The decoded value, or nullopt if text is not a well-formed hex id.
    optional<u64> ParseHexId(std::string_view text);

    /// @brief Formats an AssetId as a canonical hex-id string.
    /// @param id  The id to format.
    /// @return The zero-padded canonical string.
    string FormatAssetId(AssetId id);

    /// @brief Parses a canonical hex-id string into an AssetId.
    /// @param text  The candidate hex-id string.
    /// @return The decoded AssetId, or nullopt if text is not a well-formed hex id.
    optional<AssetId> ParseAssetId(std::string_view text);
}
