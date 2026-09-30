#include "SkeletonLoader.h"

#include <Veng/Asset/Skeleton.h>

namespace Veng
{
    AssetResult<Detail::LoadJob>
    SkeletonLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                         TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                         std::span<const u8> cooked, bool /*async*/) const
    {
        Result<Skeleton> decoded = ParseCookedSkeleton(cooked);
        if (!decoded)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(decoded.error())});
        }
        return Detail::LoadJob{.Resource =
                                   Detail::RefAny(CreateRef<Skeleton>(std::move(*decoded)))};
    }
}
