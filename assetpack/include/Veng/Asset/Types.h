#pragma once

// Minimal vocabulary types for assetpack.
//
// assetpack must not pull in engine/include/Veng/Veng.h — that would put engine/include
// on the cooker's include path (the cooker never links the engine). These aliases are a
// small subset of Veng.h's, defined identically (same underlying types, no conflict in
// TUs that include both), kept in sync by hand.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Veng
{
    /// @brief Unsigned 8-bit integer.
    using u8 = uint8_t;
    /// @brief Unsigned 32-bit integer.
    using u32 = uint32_t;
    /// @brief Unsigned 64-bit integer.
    using u64 = uint64_t;
    /// @brief Platform-native unsigned size type.
    using usize = std::size_t;
    /// @brief 32-bit floating-point scalar.
    using f32 = float;

    /// @brief Alias for std::string.
    using string = std::string;
    /// @brief Alias for std::filesystem::path.
    using path = std::filesystem::path;

    /// @brief Alias for std::vector<T>.
    template <typename T>
    using vector = std::vector<T>;

    /// @brief Alias for std::optional<T>.
    template <typename T>
    using optional = std::optional<T>;

    /// @brief Expected value or error string; the standard recoverable-error carrier.
    template <typename T>
    using Result = std::expected<T, std::string>;

    /// @brief Expected void or error string; the standard recoverable-error carrier for void returns.
    using VoidResult = std::expected<void, std::string>;
}
