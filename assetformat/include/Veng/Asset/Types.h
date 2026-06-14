#pragma once

// Minimal vendored vocabulary for assetformat (planset-5 plan 02).
//
// assetformat is Vulkan-free, importer-free, and engine-independent: it must
// not pull in engine/include/Veng/Veng.h, because that would put engine/include
// on the cooker's include path too (the cooker never links the engine). These
// aliases are a small subset of Veng.h's, defined identically (same underlying
// types, so no conflict in TUs that include both), kept in sync by hand — the
// "vendor the minimal aliases" option named in plan 02.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Veng
{
    using u8 = uint8_t;
    using u32 = uint32_t;
    using u64 = uint64_t;
    using usize = std::size_t;

    using string = std::string;
    using path = std::filesystem::path;

    template <typename T>
    using vector = std::vector<T>;

    template <typename T>
    using optional = std::optional<T>;

    template <typename T>
    using Result = std::expected<T, std::string>;

    using VoidResult = std::expected<void, std::string>;
}
