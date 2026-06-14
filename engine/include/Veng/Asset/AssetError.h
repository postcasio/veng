#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>

#include <expected>

// The structured asset-load error. AssetManager::LoadSync
// returns AssetResult<T> = std::expected<T, AssetLoadError> rather than the
// string-error Veng::Result, so callers can branch on AssetError::Kind without
// string-matching Detail.

namespace Veng
{
    enum class AssetError
    {
        NotFound,         // id not present in any mounted archive
        WrongType,        // id resolves, but to a different AssetType than requested
        Corrupt,          // cooked blob failed to parse (malformed header/payload)
        VersionMismatch,  // cooked blob's format version is not one this engine reads
        MissingDependency,// a referenced AssetId (e.g. a material's texture) isn't mounted
        LoadFailed,       // loader-specific failure (e.g. no loader registered for the type)
    };

    struct AssetLoadError
    {
        AssetError Kind;
        AssetId Id;
        string Detail;
    };

    template <typename T>
    using AssetResult = std::expected<T, AssetLoadError>;
}
