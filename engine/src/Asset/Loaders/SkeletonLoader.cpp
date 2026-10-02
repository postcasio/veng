#include "SkeletonLoader.h"

#include <Veng/Asset/Skeleton.h>

namespace Veng
{
    AssetResult<Detail::ParsedAsset> SkeletonLoader::Parse(const AssetParseContext& /*context*/,
                                                           const AssetId id,
                                                           const std::span<const u8> cooked) const
    {
        Result<Skeleton> decoded = ParseCookedSkeleton(cooked);
        if (!decoded)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(decoded.error())});
        }
        return Detail::ParsedJob(
            Detail::LoadJob{.Resource = Detail::RefAny(CreateRef<Skeleton>(std::move(*decoded)))});
    }
}
