#include "RawAssetLoader.h"

namespace Veng
{
    AssetResult<Detail::ParsedAsset> RawAssetLoader::Parse(const AssetParseContext& /*context*/,
                                                           const AssetId id,
                                                           const std::span<const u8> cooked) const
    {
        return Detail::ParsedJob(Detail::LoadJob{
            .Resource = Detail::RefAny(CreateRef<RawAsset>(RawAsset{
                .Bytes = vector<u8>(cooked.begin(), cooked.end()),
            })),
        });
    }
}
