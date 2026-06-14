#pragma once

// Minimal additional vocabulary for the cooker (planset-5 plan 03), layered on
// top of assetformat's vendored aliases (Veng::u8/u32/u64/usize, vector, path,
// string, optional, Result, VoidResult — all visible here via enclosing-
// namespace lookup, since Veng::Cook nests inside Veng). Adds the owning-
// pointer alias and the JSON type used throughout the cooker's importer
// interface.

#include <Veng/Asset/Types.h>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace Veng
{
    // function alias for the cooker (matches Veng.h's alias). Defined here so
    // cooker public headers (Importer.h) can use it without pulling in the full
    // engine Veng.h, and without leaking std:: into the house-style surface.
    template <typename T>
    using function = std::function<T>;
}

namespace Veng::Cook
{
    using json = nlohmann::json;

    template <typename T>
    using Unique = std::unique_ptr<T>;

    template <typename T, typename... Args>
    Unique<T> CreateUnique(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
}
