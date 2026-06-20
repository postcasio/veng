#pragma once

// Minimal vocabulary additions for the cooker, layered on top of assetpack's
// aliases (Veng::u8/u32/u64/usize, vector, path, string, optional, Result,
// VoidResult — visible here via enclosing-namespace lookup). Adds the owning-
// pointer alias and the JSON type used throughout the importer interface.

#include <Veng/Asset/Types.h>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace Veng
{
    /// @brief House-style `std::function` alias for cooker headers.
    ///
    /// Mirrors the alias in Veng.h so cooker public headers (e.g. Importer.h)
    /// can use `function<T>` without pulling in the full engine header.
    template <typename T>
    using function = std::function<T>;
}

namespace Veng::Cook
{
    /// @brief nlohmann::json alias used throughout the cooker's importer interface.
    using json = nlohmann::json;

    /// @brief Single-owner smart pointer alias (`std::unique_ptr<T>`).
    template <typename T>
    using Unique = std::unique_ptr<T>;

    /// @brief Constructs a Unique<T> from forwarded arguments.
    /// @tparam T     Type to construct.
    /// @tparam Args  Constructor argument types.
    template <typename T, typename... Args>
    Unique<T> CreateUnique(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
}
