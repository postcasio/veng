#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetError.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>

#include <span>

// The engine-side loader table: one AssetLoader per
// AssetType, registered into AssetManager and dispatched from LoadSync. This
// is the only place a type touches Context — mirrors how the cooker keeps its
// GPU-free Cook() separate (Veng::Cook::AssetImporter).

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    class AssetManager;

    class AssetLoader
    {
    public:
        virtual ~AssetLoader() = default;

        [[nodiscard]] virtual AssetType Type() const = 0;

        // Cooked blob (assetformat layout) -> live engine resource, type-erased
        // as Detail::RefAny (AssetHandle<T> downcasts it). May call
        // manager.LoadSync<...> to resolve dependencies (synchronous and eager
        // — a MissingDependency is an AssetLoadError, not a crash).
        [[nodiscard]] virtual AssetResult<Detail::RefAny> Load(
            AssetManager& manager, Renderer::Context& context,
            AssetId id, std::span<const u8> cooked) const = 0;
    };
}
