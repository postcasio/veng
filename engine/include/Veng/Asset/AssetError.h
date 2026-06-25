#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>

#include <expected>

namespace Veng
{
    /// @brief Structured error code for a failed asset load.
    ///
    /// Returned inside AssetLoadError so callers can branch on the kind without
    /// string-matching the Detail message.
    enum class AssetError
    {
        /// @brief Id not present in any mounted archive.
        NotFound,
        /// @brief Id resolves, but to a different AssetType than requested.
        WrongType,
        /// @brief Cooked blob failed to parse (malformed header/payload).
        Corrupt,
        /// @brief Cooked blob's format version is not one this engine reads.
        VersionMismatch,
        /// @brief A referenced AssetId (e.g. a material's texture) isn't mounted.
        MissingDependency,
        /// @brief Loader-specific failure (e.g. no loader registered for the type).
        LoadFailed,
        /// @brief The device cannot use the cooked encoding (e.g. a BC texture on a non-BC device).
        ///
        /// A hard condition: the runtime does not transcode, so a pack cooked in a codec the host
        /// GPU lacks is unloadable rather than substituted.
        Unsupported,
    };

    /// @brief Structured load-failure carrying the error kind, the failing id, and a detail message.
    struct AssetLoadError
    {
        /// @brief The error category.
        AssetError Kind;
        /// @brief The id that failed to load.
        AssetId Id;
        /// @brief Human-readable detail (not for programmatic branching; use Kind).
        string Detail;
    };

    /// @brief Expected-based result of AssetManager::LoadSync — either a resident handle or an AssetLoadError.
    template <typename T>
    using AssetResult = std::expected<T, AssetLoadError>;
}
